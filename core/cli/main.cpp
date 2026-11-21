#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "cfcore/csv_io.hpp"
#include "cfcore/date.hpp"
#include "cfcore/engine.hpp"

#include "model_input.hpp"
#include "prepayment_cli.hpp"

using namespace cfcore;

namespace {

constexpr size_t kBatchSize = 100000;
constexpr size_t kFormatBlock = 65536;  // строк cashflows.csv на поток за один проход
constexpr size_t kMaxRowLength = 160;

struct Arguments {
    std::string input;
    std::string output_dir;
    std::string fixings;
    int threads = 0;
    std::optional<Policy::Mode> mode;
    bool write_flows = true;
    bool details_only = false;
    bool write_events = false;
    std::optional<int64_t> trace_deal;
    bool round_output = false;
    bool risk_buckets = false;
    std::string risk_coupons;
    prepayment_cli::Arguments prepayment;
};

void print_usage() {
    std::cout <<
        "Использование: cfcore --input deals.csv --output-dir out [опции]\n"
        "  --fixings FILE    история индикатора (date,value)\n"
        "  --threads N       число потоков, по умолчанию все ядра\n"
        "  --excel-etalon    правила листа \"1 сделка\"\n"
        "  --policy NAME     base, legacy-excel или contractual-v2\n"
        "  --no-flows        не писать cashflows.csv\n"
        "  --events          писать events.csv с событиями пересмотра ставки\n"
        "  --trace-deal ID   писать deal_trace.csv для одной сделки\n"
        "  --details-only    не писать buckets.csv, errors.csv и манифест, без сводки на экране\n"
        "  --risk-buckets 8mr    отдельные регуляторные бакеты и детализация\n"
        "  --risk-coupons FILE  определённость и суммы FLOAT-купонов\n"
        "  --prepayment-model none|bvdpk   модель досрочного погашения\n"
        "  --prepayment-eligible FILE      однородные портфели и правило пересчёта\n"
        "  --bvdpk RATE | --bvdpk-file FILE  доля за 30 дней\n"
        "  --bvdpk-source demonstration|bank\n"
        "  --prepayment-scenario NAME       сценарий БВДПК, по умолчанию base\n"
        "  --round-money     округлить суммы до копеек при выводе\n";
}

Arguments parse_arguments(std::span<const std::string> argv) {
    Arguments args;
    const auto set_mode = [&](Policy::Mode mode) {
        if (args.mode && *args.mode != mode) {
            throw std::runtime_error("заданы несовместимые расчётные политики");
        }
        args.mode = mode;
    };
    for (size_t i = 1; i < argv.size(); ++i) {
        const std::string& name = argv[i];
        const auto value = [&]() -> const std::string& {
            if (i + 1 >= argv.size()) {
                throw std::runtime_error("нет значения для " + name);
            }
            return argv[++i];
        };
        if (name == "--input") args.input = value();
        else if (name == "--output-dir") args.output_dir = value();
        else if (name == "--fixings") args.fixings = value();
        else if (name == "--threads") args.threads = std::stoi(value());
        else if (name == "--excel-etalon") set_mode(Policy::Mode::LegacyExcel);
        else if (name == "--policy") {
            const auto& text = value();
            if (text == "base") set_mode(Policy::Mode::Base);
            else if (text == "legacy-excel") set_mode(Policy::Mode::LegacyExcel);
            else if (text == "contractual-v2") set_mode(Policy::Mode::ContractualV2);
            else throw std::runtime_error("неизвестная расчётная политика: " + text);
        } else if (name == "--no-flows") args.write_flows = false;
        else if (name == "--events") args.write_events = true;
        else if (name == "--trace-deal") {
            const std::string& text = value();
            int64_t id = 0;
            const auto end = text.data() + text.size();
            const auto result = std::from_chars(text.data(), end, id);
            if (result.ec != std::errc{} || result.ptr != end) {
                throw std::runtime_error("неверный instrument_id для --trace-deal: " + text);
            }
            args.trace_deal = id;
        } else if (name == "--details-only") args.details_only = true;
        else if (name == "--risk-buckets") {
            if (value() != "8mr") throw std::runtime_error("--risk-buckets допускает только 8mr");
            args.risk_buckets = true;
        } else if (name == "--risk-coupons") args.risk_coupons = value();
        else if (name == "--prepayment-model") {
            const auto& model = value();
            if (model != "none" && model != "bvdpk") throw std::runtime_error("unknown prepayment model");
            args.prepayment.enabled = model == "bvdpk";
        } else if (name == "--bvdpk") args.prepayment.bvdpk = value();
        else if (name == "--bvdpk-file") args.prepayment.bvdpk_file = value();
        else if (name == "--prepayment-eligible") args.prepayment.eligibility_file = value();
        else if (name == "--prepayment-scenario") args.prepayment.scenario = value();
        else if (name == "--bvdpk-source") args.prepayment.source = value();
        else if (name == "--round-money") args.round_output = true;
        else throw std::runtime_error("неизвестная опция " + name);
    }
    if (args.input.empty() || args.output_dir.empty()) {
        throw std::runtime_error("нужны --input и --output-dir");
    }
    if ((!args.risk_buckets && !args.risk_coupons.empty()) ||
        (args.risk_buckets && args.details_only)) {
        throw std::runtime_error("--risk-coupons требует --risk-buckets 8mr, несовместимый с --details-only");
    }
    const auto& prepayment = args.prepayment;
    if ((!prepayment.enabled && (prepayment.bvdpk || !prepayment.bvdpk_file.empty() ||
         !prepayment.eligibility_file.empty() || prepayment.scenario || prepayment.source)) ||
        (prepayment.enabled && args.details_only)) {
        throw std::runtime_error("Параметры БВДПК требуют --prepayment-model bvdpk без --details-only");
    }
    return args;
}

std::string utf8_path(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

std::string json_string(const std::string& text) {
    constexpr char hex[] = "0123456789abcdef";
    std::string quoted = "\"";
    for (unsigned char byte : text) {
        if (byte == '"' || byte == '\\') {
            quoted += '\\';
            quoted += static_cast<char>(byte);
        } else if (byte < 0x20) {
            quoted += "\\u00";
            quoted += hex[byte >> 4];
            quoted += hex[byte & 0x0f];
        } else {
            quoted += static_cast<char>(byte);
        }
    }
    quoted += '"';
    return quoted;
}

// Файл закрывается автоматически, ошибка открытия останавливает запуск.
using File = std::unique_ptr<FILE, int (*)(FILE*)>;

File open_output(const std::filesystem::path& path) {
#ifdef _WIN32
    File file(_wfopen(path.c_str(), L"w"), std::fclose);
#else
    File file(std::fopen(path.c_str(), "w"), std::fclose);
#endif
    if (!file) {
        throw std::runtime_error("не удалось создать файл: " + utf8_path(path));
    }
    return file;
}

char* append_fixed(char* out, double value) {
    return std::to_chars(out, out + 64, value, std::chars_format::fixed, 10).ptr;
}

char* append_digits(char* out, int value, int width) {
    for (int i = width - 1; i >= 0; --i) {
        out[i] = static_cast<char>('0' + value % 10);
        value /= 10;
    }
    return out + width;
}

void append_flow(std::string& text, const CashFlow& flow, bool round_output) {
    char row[kMaxRowLength];
    char* end = std::to_chars(row, row + 32, flow.instrument_id).ptr;
    *end++ = ',';
    const YearMonthDay ymd = to_ymd(flow.date);
    end = append_digits(end, ymd.year, 4);
    *end++ = '-';
    end = append_digits(end, ymd.month, 2);
    *end++ = '-';
    end = append_digits(end, ymd.day, 2);
    const char* type = flow.type == FlowType::Principal ? ",PRINCIPAL," : ",INTEREST,";
    end = std::copy(type, type + std::strlen(type), end);
    end = append_fixed(end, round_output ? round_money(flow.amount) : flow.amount);
    *end++ = ',';
    end = append_fixed(end, flow.rate_used);
    *end++ = ',';
    end = append_fixed(end, flow.year_fraction);
    *end++ = '\n';
    text.append(row, end);
}

// Текст готовят все потоки, а в файл части идут строго по порядку, поэтому
// содержимое не зависит от числа потоков.
void write_flows(FILE* file, std::span<const CashFlow> flows, int threads, bool round_output) {
    std::vector<std::string> parts(threads);
    for (std::string& part : parts) {
        part.reserve(kFormatBlock * kMaxRowLength);
    }
    while (!flows.empty()) {
        std::vector<std::thread> workers;
        int used = 0;
        for (; used < threads && !flows.empty(); ++used) {
            const auto slice = flows.first(std::min(kFormatBlock, flows.size()));
            flows = flows.subspan(slice.size());
            parts[used].clear();
            workers.emplace_back([slice, round_output, &part = parts[used]] {
                for (const CashFlow& flow : slice) {
                    append_flow(part, flow, round_output);
                }
            });
        }
        for (std::thread& worker : workers) {
            worker.join();
        }
        for (int i = 0; i < used; ++i) {
            if (std::fwrite(parts[i].data(), 1, parts[i].size(), file) != parts[i].size()) {
                throw std::runtime_error("не удалось записать cashflows.csv, проверьте место на диске");
            }
        }
    }
}

const char* event_name(EventType type) {
    switch (type) {
        case EventType::InterestPayment: return "INTEREST";
        case EventType::PrincipalPayment: return "PRINCIPAL";
        case EventType::RateReset: return "REPRICING";
    }
    return "";
}

const char* rate_source_name(RateSource source) {
    switch (source) {
        case RateSource::None: return "";
        case RateSource::ContractRate: return "CONTRACT_RATE";
        case RateSource::ContractSchedule: return "CONTRACT_SCHEDULE";
        case RateSource::FixingHistory: return "FIXING_HISTORY";
        case RateSource::DealIndicator: return "DEAL_INDICATOR";
    }
    return "";
}

void write_repricing(FILE* file, const EventDetail& event) {
    std::fprintf(file, "%lld,%s,REPRICING,%.17g,%.17g,%s,0\n",
                 static_cast<long long>(event.instrument_id), format_date(event.date).c_str(),
                 event.rate_before, event.rate_after, rate_source_name(event.rate_source));
}

void write_trace(FILE* file, const EventDetail& event) {
    std::fprintf(file, "%lld,%s,%s,%s,%s,%d,%.17g,%.17g,%.17g,%.17g,%.17g,%s,%.17g,%.17g,%.17g,%.17g",
                 static_cast<long long>(event.instrument_id), format_date(event.date).c_str(),
                 event_name(event.type), format_date(event.period_start).c_str(),
                 format_date(event.period_end).c_str(), event.period_end - event.period_start,
                 event.year_fraction, event.balance_before, event.balance_after, event.rate_before,
                 event.rate_after, rate_source_name(event.rate_source), event.interest_accrued,
                 event.interest_paid, event.principal_paid, event.interest_paid + event.principal_paid);
    std::fprintf(file, ",%s,", rate_source_name(event.rate_source_before));
    if (event.last_repricing_date) {
        std::fputs(format_date(*event.last_repricing_date).c_str(), file);
    }
    for (const auto& value : {event.accrued_interest_included, event.pmt_before, event.pmt_after}) {
        std::fputc(',', file);
        if (value) {
            std::fprintf(file, "%.17g", *value);
        }
    }
    std::fputc('\n', file);
}

const char* side_name(Side side) {
    return side == Side::Asset ? "ASSET" : "LIABILITY";
}

RiskCoupons read_risk_coupons(const std::string& filename) {
    RiskCoupons result;
    if (!filename.empty()) {
        model_input::csv(filename, "instrument_id,payment_date,known_interest", 3, [&](const auto& fields) {
            const auto id = model_input::number<int64_t>(fields[0], "instrument_id");
            Date date{};
            if (!parse_date(fields[1], date)) throw std::runtime_error("invalid payment_date");
            std::optional<double> amount;
            if (!fields[2].empty()) {
                amount = model_input::number<double>(fields[2], "known_interest");
                if (*amount < 0) throw std::runtime_error("known_interest must be nonnegative");
            }
            if (!result.emplace(std::pair{id, date}, amount).second) {
                throw std::runtime_error("duplicate coupon");
            }
        });
    }
    return result;
}

bool write_risk_allocation(FILE* file, const RiskAllocation& row, Accumulator& totals) {
    const Bucketizer bucketizer(row.report_date);
    std::fprintf(file, "%lld,%d,RUB,%s,", static_cast<long long>(row.instrument_id),
                 row.portfolio_key, side_name(row.side));
    std::fputs(format_date(row.report_date).c_str(), file);
    std::fputc(',', file);
    std::fputs(format_date(row.payment_date).c_str(), file);
    std::fprintf(file, ",%s,%.17g,", row.type == FlowType::Principal ? "PRINCIPAL" : "INTEREST",
                 row.original_amount);
    if (row.next_planned_repricing) std::fputs(format_date(*row.next_planned_repricing).c_str(), file);
    std::fputc(',', file);
    if (row.risk_date) {
        std::fputs(format_date(*row.risk_date).c_str(), file);
        std::fprintf(file, ",%d,%d,%.17g", *row.risk_date - row.report_date,
                     bucketizer.bucket(*row.risk_date), row.risk_amount);
        const CashFlow weighted{row.instrument_id, *row.risk_date, row.type, row.risk_amount};
        totals.add({row.portfolio_key, Currency::RUB, row.side}, std::span(&weighted, 1), bucketizer);
    } else {
        std::fprintf(file, ",,,");
    }
    std::fprintf(file, ",%s,%s\n", row.coupon_status, to_string(row.rule));
    return row.risk_date.has_value();
}

int run_cli(std::span<const std::string> argv) {
    if (argv.size() == 1 || argv[1] == "--help") {
        print_usage();
        return argv.size() == 1 ? 1 : 0;
    }
    try {
        const auto started = std::chrono::steady_clock::now();
        const Arguments args = parse_arguments(argv);
        const int threads = args.threads > 0 ? args.threads
                                             : static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
        const bool excel = args.mode == Policy::Mode::LegacyExcel;
        const bool contractual = args.mode == Policy::Mode::ContractualV2;
        const Policy policy = contractual ? Policy::contractual_v2() : excel ? Policy::excel_etalon() : Policy{};
        const char* mode = contractual ? "CONTRACTUAL_V2" : excel ? "EXCEL_ETALON" : "BASE";
        const char* policy_name = excel ? "LEGACY_EXCEL" : mode;
        const auto money = [&](double value) { return args.round_output ? round_money(value) : value; };

        const std::filesystem::path out_dir(std::u8string(args.output_dir.begin(), args.output_dir.end()));
        std::filesystem::create_directories(out_dir);

        std::optional<prepayment_cli::Configuration> prepayment;
        if (args.prepayment.enabled) {
            std::filesystem::remove(out_dir / "behavioral_buckets.csv");
            prepayment = prepayment_cli::read_configuration(args.prepayment);
        }
        std::map<int32_t, PrepaymentPortfolio> prepayment_portfolios;
        const RiskCoupons risk_coupons = read_risk_coupons(args.risk_coupons);
        Accumulator risk_totals;
        size_t unresolved_risk_rows = 0;
        File risk_file(nullptr, std::fclose);
        if (args.risk_buckets) {
            // A failed rerun must not leave an apparently valid aggregate from an older run.
            std::filesystem::remove(out_dir / "risk_buckets.csv");
            risk_file = open_output(out_dir / "risk_bucket_allocations.csv");
            std::fprintf(risk_file.get(), "instrument_id,portfolio_key,currency,side,report_date,payment_date,"
                         "flow_type,original_amount,next_planned_repricing,risk_date,risk_term_days,"
                         "bucket,risk_amount,coupon_status,rule\n");
        }

        File flows_file(nullptr, std::fclose);
        if (args.write_flows) {
            flows_file = open_output(out_dir / "cashflows.csv");
            std::fprintf(flows_file.get(),
                         "instrument_id,payment_date,cash_flow_type,amount,rate_used,year_fraction\n");
        }
        File events_file(nullptr, std::fclose);
        if (args.write_events) {
            events_file = open_output(out_dir / "events.csv");
            std::fprintf(events_file.get(),
                         "instrument_id,event_date,event_type,rate_before,rate_after,rate_source,cash_amount\n");
        }
        File trace_file(nullptr, std::fclose);
        if (args.trace_deal) {
            trace_file = open_output(out_dir / "deal_trace.csv");
            std::fprintf(trace_file.get(),
                         "instrument_id,event_date,event_type,period_start,period_end,days,year_fraction,"
                         "balance_before,balance_after,rate_before,rate_after,rate_source,interest_accrued,"
                         "interest_paid,principal_paid,cash_amount,rate_source_before,last_repricing_date,"
                         "accrued_interest_included,pmt_before,pmt_after\n");
        }
        File summary_file = open_output(out_dir / "deal_summary.csv");
        std::fprintf(summary_file.get(),
                     "instrument_id,opening_principal,total_principal,total_interest,closing_balance,"
                     "flow_count,current_interest,current_interest_source\n");

        DealArena arena;
        if (!args.fixings.empty()) {
            arena.indicator_fixings = read_fixings(args.fixings);
            arena.external_fixings = true;
        }

        CsvDealReader reader(args.input);
        std::vector<Deal> deals;
        std::vector<DealError> errors;
        Accumulator accumulator;
        size_t deals_ok = 0, flow_count = 0;
        RunOptions options;
        options.threads = threads;
        options.keep_flows = args.write_flows;
        options.keep_repricing = args.write_events;
        options.trace_deal = args.trace_deal;
        options.risk_coupons = args.risk_buckets ? &risk_coupons : nullptr;
        options.prepayment_rates = prepayment ? &prepayment->rates : nullptr;

        // Порция считается, результат записывается, память порции переиспользуется.
        while (reader.next_batch(kBatchSize, deals, arena, errors)) {
            PortfolioResult result = run_portfolio(deals, arena, policy, options);
            accumulator.merge(result.accumulator);
            errors.insert(errors.end(), result.errors.begin(), result.errors.end());
            deals_ok += result.summaries.size();
            for (const auto& [key, portfolio] : result.prepayment_portfolios) {
                prepayment_portfolios[key].merge(portfolio);
            }
            if (args.write_flows) {
                write_flows(flows_file.get(), result.flows, threads, args.round_output);
            }
            for (const EventDetail& event : result.event_details) {
                if (events_file && event.type == EventType::RateReset) {
                    write_repricing(events_file.get(), event);
                }
                if (trace_file && args.trace_deal == event.instrument_id) {
                    write_trace(trace_file.get(), event);
                }
            }
            for (const RiskAllocation& row : result.risk_allocations) {
                if (!write_risk_allocation(risk_file.get(), row, risk_totals)) ++unresolved_risk_rows;
            }
            for (const DealSummary& s : result.summaries) {
                flow_count += s.flow_count;
                std::fprintf(summary_file.get(), "%lld,%.10f,%.10f,%.10f,%.10f,%d,",
                             static_cast<long long>(s.instrument_id), money(s.opening_principal),
                             money(s.total_principal), money(s.total_interest), money(s.closing_balance),
                             s.flow_count);
                if (s.current_interest) {
                    std::fprintf(summary_file.get(), "%.10f", money(*s.current_interest));
                }
                std::fprintf(summary_file.get(), ",%s\n", to_string(s.current_interest_source));
            }
        }

        if (args.details_only) {
            // Без errors.csv пропуск сделок был бы незаметен, поэтому о нём сообщаем всегда.
            if (!errors.empty()) {
                std::map<std::string, size_t> counts;
                for (const DealError& error : errors) {
                    ++counts[to_string(error.code)];
                }
                std::fprintf(stderr, "не посчитано сделок: %zu", errors.size());
                for (const auto& [code, count] : counts) {
                    std::fprintf(stderr, ", %s %zu", code.c_str(), count);
                }
                std::fprintf(stderr, "\n");
            }
            return 0;
        }

        File buckets_file = open_output(out_dir / "buckets.csv");
        std::fprintf(buckets_file.get(), "portfolio_key,currency,side,bucket,principal_cf,interest_cf\n");
        for (const auto& [key, totals] : accumulator.totals()) {
            for (int k = 0; k < kBucketCount; ++k) {
                if (totals.principal[k] != 0.0 || totals.interest[k] != 0.0) {
                    std::fprintf(buckets_file.get(), "%d,RUB,%s,%d,%.10f,%.10f\n", key.portfolio_key,
                                 side_name(key.side), k, money(totals.principal[k]), money(totals.interest[k]));
                }
            }
        }

        std::sort(errors.begin(), errors.end(),
                  [](const DealError& a, const DealError& b) { return a.instrument_id < b.instrument_id; });
        File errors_file = open_output(out_dir / "errors.csv");
        std::fprintf(errors_file.get(), "instrument_id,error_code\n");
        for (const DealError& error : errors) {
            std::fprintf(errors_file.get(), "%lld,%s\n", static_cast<long long>(error.instrument_id),
                         to_string(error.code));
        }

        const bool risk_complete = unresolved_risk_rows == 0 && errors.empty();
        if (args.risk_buckets && risk_complete) {
            File buckets = open_output(out_dir / "risk_buckets.csv");
            std::fprintf(buckets.get(), "portfolio_key,currency,side,bucket,principal_cf,interest_cf\n");
            for (const auto& [key, totals] : risk_totals.totals()) {
                for (int k = 0; k < kBucketCount; ++k) {
                    std::fprintf(buckets.get(), "%d,RUB,%s,%d,%.17g,%.17g\n", key.portfolio_key,
                                 side_name(key.side), k, totals.principal[k], totals.interest[k]);
                }
            }
        }

        std::map<int32_t, std::array<BehavioralBucket, kBucketCount>> projections;
        std::map<int32_t, std::string> prepayment_errors;
        if (prepayment) {
            for (const auto& [key, rate] : prepayment->rates) {
                const auto portfolio = prepayment_portfolios.find(key);
                if (portfolio == prepayment_portfolios.end()) {
                    prepayment_errors[key] = "MISSING_PORTFOLIO";
                } else if (const char* reason = portfolio->second.error()) {
                    prepayment_errors[key] = reason;
                } else {
                    try {
                        const double scenario_rate = std::min(1.0, rate * prepayment->scenario.coefficient);
                        projections[key] = portfolio->second.project(scenario_rate);
                    } catch (const std::runtime_error& error) {
                        prepayment_errors[key] = error.what();
                    }
                }
            }
        }
        const bool prepayment_complete = prepayment_errors.empty() && errors.empty();
        if (prepayment && prepayment_complete) {
            File behavioral = open_output(out_dir / "behavioral_buckets.csv");
            prepayment_cli::write_bucket_header(behavioral.get());
            for (const auto& [key, buckets] : projections) {
                prepayment_cli::write_buckets(behavioral.get(), key, prepayment_portfolios.at(key).report_date(),
                                             buckets);
            }
        }

        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - started;
        File manifest = open_output(out_dir / "run_manifest.json");
        std::fprintf(manifest.get(),
                     "{\n  \"input\": %s,\n  \"mode\": \"%s\",\n  \"calculation_policy\": \"%s\",\n  \"threads\": %d,\n"
                     "  \"deals_ok\": %zu,\n  \"deals_failed\": %zu,\n  \"flows\": %zu,\n"
                     "  \"seconds\": %.3f,\n  \"policy\": {\n    \"force_act365\": %s,\n"
                     "    \"close_residual\": %s,\n    \"first_period_from_prev_pay\": %s,\n"
                     "    \"include_payment_on_report\": %s,\n    \"float_rate_is_spread\": %s\n  }",
                     json_string(args.input).c_str(), mode, policy_name, threads, deals_ok,
                     errors.size(), flow_count, elapsed.count(), policy.force_act365 ? "true" : "false",
                     policy.close_residual ? "true" : "false",
                     policy.first_period_from_prev_pay ? "true" : "false",
                     policy.include_payment_on_report ? "true" : "false",
                     policy.float_rate_is_spread ? "true" : "false");

        if (args.risk_buckets) {
            std::fprintf(manifest.get(), ",\n  \"risk_buckets\": {\"method\": \"8mr\", \"complete\": %s, "
                         "\"unresolved_rows\": %zu, \"contract_errors\": %zu}\n",
                         risk_complete ? "true" : "false", unresolved_risk_rows, errors.size());
        }
        if (prepayment) {
            const std::string value_source = args.prepayment.bvdpk_file.empty()
                                               ? "cli" : args.prepayment.bvdpk_file;
            std::fprintf(manifest.get(), ",\n  \"prepayment\": {\"enabled\": true, \"model\": \"bvdpk\", "
                         "\"complete\": %s, \"demonstration\": %s, \"source\": %s, \"eligibility\": %s, "
                         "\"scenario\": \"%s\", \"coefficient\": %.17g, \"calculation_policy\": \"%s\", "
                         "\"contract_errors\": %zu, \"portfolios\": [",
                         prepayment_complete ? "true" : "false", prepayment->demonstration ? "true" : "false",
                         json_string(value_source).c_str(), json_string(args.prepayment.eligibility_file).c_str(),
                         prepayment->scenario.name.data(), prepayment->scenario.coefficient, policy_name, errors.size());
            const char* separator = "";
            for (const auto& [key, rate] : prepayment->rates) {
                std::fprintf(manifest.get(), "%s{\"portfolio_key\": %d, \"bvdpk\": %.17g, \"rvdpk\": %.17g",
                             separator, key, rate, std::min(1.0, rate * prepayment->scenario.coefficient));
                if (const auto error = prepayment_errors.find(key); error != prepayment_errors.end()) {
                    std::fprintf(manifest.get(), ", \"error\": %s", json_string(error->second).c_str());
                }
                std::fputc('}', manifest.get());
                separator = ", ";
            }
            std::fprintf(manifest.get(), "]}");
        } else {
            std::fprintf(manifest.get(), ",\n  \"prepayment\": {\"enabled\": false}");
        }
        std::fprintf(manifest.get(), "\n}\n");
        if (prepayment && !prepayment_complete) {
            std::fprintf(stderr, "модель БВДПК не рассчитана, см. run_manifest.json и errors.csv\n");
        }
        if (args.risk_buckets && !risk_complete) {
            std::fprintf(stderr, "регуляторная агрегация не завершена, см. risk_bucket_allocations.csv и errors.csv\n");
            return 3;
        }

        if (prepayment && !prepayment_complete) return 3;

        std::printf("сделок рассчитано: %zu, с ошибкой: %zu, потоков: %zu, потоков ЦП: %d, время: %.3f с\n",
                    deals_ok, errors.size(), flow_count, threads, elapsed.count());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "ошибка: %s\n", error.what());
        return 2;
    }
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> arguments;
    arguments.reserve(argc);
    for (int i = 0; i < argc; ++i) {
        arguments.push_back(utf8_path(argv[i]));
    }
    return run_cli(arguments);
}
#else
int main(int argc, char** argv) {
    const std::vector<std::string> arguments(argv, argv + argc);
    return run_cli(arguments);
}
#endif
