#include "cfcore/csv_io.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

#include "cfcore/date.hpp"

namespace cfcore {

namespace {

constexpr char kDelimiter = ';';

const char* const kRequiredColumns[] = {
    "deal_id", "report_dttm", "discriminator", "currency_id", "side", "portfolio_key",
    "current_principal_amt", "deal_dttm", "maturity_dttm", "rate_type_cd", "interest_rate",
    "base_indicator_rate", "cap_limit_interest_rate", "floor_limit_interest_rate",
    "accrued_interest", "day_convention_cd",
    "interest_payment_rule_start_dttm", "interest_payment_rule_period_cd", "interest_payment_rule_day_num",
    "regular_payment_rule_start_dttm", "regular_payment_rule_period_cd", "regular_payment_rule_day_num",
    "repricing_rule_start_dttm", "repricing_rule_period_cd", "repricing_rule_day_num",
    "interest_payment", "principal_payment", "repricing_dttm_schedule",
    "repricing_dttm_rate_schedule_fixed",
};

// Ошибка разбора одной строки: файл продолжает читаться.
struct RowError {};

std::vector<std::string> split(const std::string& line, char delimiter) {
    std::vector<std::string> cells;
    std::string cell;
    std::stringstream stream(line);
    while (std::getline(stream, cell, delimiter)) {
        cells.push_back(cell);
    }
    if (!line.empty() && line.back() == delimiter) {
        cells.emplace_back();
    }
    return cells;
}

// Строка без завершающего \r: файлы из Windows и Python приходят с CRLF.
bool read_line(std::istream& stream, std::string& line) {
    if (!std::getline(stream, line)) {
        return false;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return true;
}

double require_double(const std::string& text) {
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (text.empty() || *end != '\0') {
        throw RowError{};
    }
    return value;
}

int64_t require_int(const std::string& text) {
    char* end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (text.empty() || *end != '\0') {
        throw RowError{};
    }
    return value;
}

std::optional<double> optional_double(const std::string& text) {
    return text.empty() ? std::nullopt : std::optional<double>(require_double(text));
}

Date require_date(const std::string& text) {
    Date date = 0;
    if (!parse_date(text, date)) {
        throw RowError{};
    }
    return date;
}

// Формат периода: 1M, 45D или ENDDATE. Пусто: правила нет.
PaymentRule parse_rule(const std::string& start, const std::string& period, const std::string& day,
                       Date default_start) {
    PaymentRule rule;
    if (period.empty()) {
        return rule;
    }
    rule.start = start.empty() ? default_start : require_date(start);
    rule.day = day.empty() ? 0 : static_cast<int32_t>(require_int(day));
    if (period == "ENDDATE") {
        rule.kind = PaymentRule::Kind::EndDate;
        return rule;
    }
    const char unit = period.back();
    if (unit != 'M' && unit != 'D') {
        throw RowError{};
    }
    rule.kind = unit == 'M' ? PaymentRule::Kind::Months : PaymentRule::Kind::Days;
    rule.period = static_cast<int32_t>(require_int(period.substr(0, period.size() - 1)));
    return rule;
}

// Списки разделены пробелами, пара внутри элемента: дата,значение.
Span parse_dates(const std::string& text, std::vector<Date>& target) {
    const auto offset = static_cast<uint32_t>(target.size());
    for (const std::string& item : split(text, ' ')) {
        if (!item.empty()) {
            target.push_back(require_date(item));
        }
    }
    return {offset, static_cast<uint32_t>(target.size() - offset)};
}

Span parse_principal(const std::string& text, std::vector<PrincipalItem>& target) {
    const auto offset = static_cast<uint32_t>(target.size());
    for (const std::string& item : split(text, ' ')) {
        if (item.empty()) {
            continue;
        }
        const auto parts = split(item, ',');
        if (parts.size() != 2) {
            throw RowError{};
        }
        target.push_back({require_date(parts[0]), require_double(parts[1])});
    }
    return {offset, static_cast<uint32_t>(target.size() - offset)};
}

Span parse_resets(const std::string& dates, const std::string& fixed, std::vector<ResetItem>& target) {
    const auto offset = static_cast<uint32_t>(target.size());
    for (const std::string& item : split(dates, ' ')) {
        if (!item.empty()) {
            target.push_back({require_date(item), std::nullopt});
        }
    }
    for (const std::string& item : split(fixed, ' ')) {
        if (item.empty()) {
            continue;
        }
        const auto parts = split(item, ',');
        if (parts.size() != 2) {
            throw RowError{};
        }
        target.push_back({require_date(parts[0]), require_double(parts[1])});
    }
    std::stable_sort(target.begin() + offset, target.end(),
                     [](const ResetItem& a, const ResetItem& b) { return a.date < b.date; });
    return {offset, static_cast<uint32_t>(target.size() - offset)};
}

AmortizationType parse_amortization(const std::string& text) {
    if (text == "ANN") return AmortizationType::ANN;
    if (text == "RGM") return AmortizationType::RGM;
    if (text == "SIMPLE") return AmortizationType::SIMPLE;
    throw RowError{};
}

RateType parse_rate_type(const std::string& text) {
    if (text == "FIXED") return RateType::Fixed;
    if (text == "FLOAT") return RateType::Float;
    throw RowError{};
}

DayCount parse_day_count(const std::string& text) {
    if (text == "ACT_365") return DayCount::Act365;
    if (text == "ACT_ACT_ISDA") return DayCount::ActActIsda;
    throw RowError{};
}

Side parse_side(const std::string& text) {
    if (text == "ASSET") return Side::Asset;
    if (text == "LIABILITY") return Side::Liability;
    throw RowError{};
}

}  // namespace

bool parse_date(const std::string& text, Date& date) {
    int year = 0, month = 0, day = 0;
    char extra = 0;
    if (text.size() != 10 ||
        std::sscanf(text.c_str(), "%4d-%2d-%2d%c", &year, &month, &day, &extra) != 3) {
        return false;
    }
    if (month < 1 || month > 12 || day < 1 || day > days_in_month(year, month)) {
        return false;
    }
    date = from_ymd(year, month, day);
    return true;
}

std::string format_date(Date date) {
    const YearMonthDay ymd = to_ymd(date);
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", ymd.year, ymd.month, ymd.day);
    return buffer;
}

CsvDealReader::CsvDealReader(const std::string& path) : file_(path) {
    if (!file_) {
        throw std::runtime_error("не удалось открыть файл: " + path);
    }
    std::string header;
    read_line(file_, header);
    const auto names = split(header, kDelimiter);
    for (size_t i = 0; i < names.size(); ++i) {
        columns_[names[i]] = i;
    }
    for (const char* column : kRequiredColumns) {
        if (columns_.count(column) == 0) {
            throw std::runtime_error(std::string("в файле нет колонки ") + column);
        }
    }
}

bool CsvDealReader::next_batch(size_t max_deals, std::vector<Deal>& deals, DealArena& arena,
                               std::vector<DealError>& errors) {
    deals.clear();
    arena.interest_dates.clear();
    arena.principal_items.clear();
    arena.reset_items.clear();

    std::string line;
    bool read_any = false;
    while (deals.size() < max_deals && read_line(file_, line)) {
        if (line.empty()) {
            continue;
        }
        read_any = true;
        auto cells = split(line, kDelimiter);
        cells.resize(columns_.size());
        const auto cell = [&](const char* name) -> const std::string& { return cells[columns_.at(name)]; };

        Deal deal;
        // Даже без остальных полей ошибка должна указывать на сделку.
        deal.instrument_id = std::strtoll(cell("deal_id").c_str(), nullptr, 10);
        const size_t interest_size = arena.interest_dates.size();
        const size_t principal_size = arena.principal_items.size();
        const size_t reset_size = arena.reset_items.size();
        try {
            if (cell("currency_id") != "RUB") {
                errors.push_back({deal.instrument_id, ErrorCode::Unsupported});
                continue;
            }
            deal.instrument_id = require_int(cell("deal_id"));
            deal.portfolio_key = static_cast<int32_t>(require_int(cell("portfolio_key")));
            deal.side = parse_side(cell("side"));
            deal.outstanding_principal = require_double(cell("current_principal_amt"));
            deal.report_date = require_date(cell("report_dttm"));
            deal.start_date = require_date(cell("deal_dttm"));
            deal.maturity_date = require_date(cell("maturity_dttm"));
            deal.amortization = parse_amortization(cell("discriminator"));
            deal.rate_type = parse_rate_type(cell("rate_type_cd"));
            deal.day_count = parse_day_count(cell("day_convention_cd"));
            deal.rate = require_double(cell("interest_rate"));
            deal.indicator_rate = optional_double(cell("base_indicator_rate"));
            deal.cap = optional_double(cell("cap_limit_interest_rate"));
            deal.floor = optional_double(cell("floor_limit_interest_rate"));
            deal.accrued_interest = optional_double(cell("accrued_interest"));
            deal.interest_rule = parse_rule(cell("interest_payment_rule_start_dttm"),
                                            cell("interest_payment_rule_period_cd"),
                                            cell("interest_payment_rule_day_num"), deal.start_date);
            deal.principal_rule = parse_rule(cell("regular_payment_rule_start_dttm"),
                                             cell("regular_payment_rule_period_cd"),
                                             cell("regular_payment_rule_day_num"), deal.start_date);
            deal.reset_rule = parse_rule(cell("repricing_rule_start_dttm"),
                                         cell("repricing_rule_period_cd"),
                                         cell("repricing_rule_day_num"), deal.start_date);
            deal.interest_schedule = parse_dates(cell("interest_payment"), arena.interest_dates);
            deal.principal_schedule = parse_principal(cell("principal_payment"), arena.principal_items);
            deal.reset_schedule = parse_resets(cell("repricing_dttm_schedule"),
                                               cell("repricing_dttm_rate_schedule_fixed"),
                                               arena.reset_items);
            deals.push_back(deal);
        } catch (const RowError&) {
            arena.interest_dates.resize(interest_size);
            arena.principal_items.resize(principal_size);
            arena.reset_items.resize(reset_size);
            errors.push_back({deal.instrument_id, ErrorCode::ParseError});
        }
    }
    return read_any;
}

std::vector<RateFixing> read_fixings(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("не удалось открыть файл: " + path);
    }
    std::vector<RateFixing> fixings;
    std::string line;
    read_line(file, line);
    while (read_line(file, line)) {
        const auto parts = split(line, ',');
        Date date = 0;
        if (parts.size() != 2 || !parse_date(parts[0], date)) {
            throw std::runtime_error("неверная строка истории ставки: " + line);
        }
        fixings.push_back({date, std::strtod(parts[1].c_str(), nullptr)});
    }
    if (!std::is_sorted(fixings.begin(), fixings.end(),
                        [](const RateFixing& a, const RateFixing& b) { return a.date < b.date; })) {
        throw std::runtime_error("история ставки не отсортирована: " + path);
    }
    return fixings;
}

}  // namespace cfcore
