#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "cfcore/deal.hpp"
#include "cfcore/event.hpp"
#include "cfcore/finance.hpp"
#include "cfcore/policy.hpp"

namespace cfcore {

enum class ErrorCode : uint8_t {
    Ok,
    Unsupported,
    InvalidNumber,
    InvalidDate,
    InvalidSchedule,
    ScheduleSumMismatch,  // сумма будущего графика больше остатка
    ScheduleEndsEarly,    // сумма будущего графика меньше остатка
    MissingRate,          // нет значения индикатора для плавающей ставки
    TooManyEvents,
    NonFiniteResult,      // в расчёте появился NaN или бесконечность
    ParseError,           // строка входного файла не разобрана
};

const char* to_string(ErrorCode code);

struct DealError {
    int64_t instrument_id = 0;
    ErrorCode code = ErrorCode::Ok;
};

enum class InterestSource : uint8_t {
    Reported,     // значение из данных, явный 0 тоже сюда
    Computed,     // посчитано ядром
    Unavailable,  // данных недостаточно, значение не подставляется
};

const char* to_string(InterestSource source);

// Итоги по сделке: из них восстанавливается проверка полного погашения.
struct DealSummary {
    int64_t instrument_id = 0;
    double opening_principal = 0.0;
    double total_principal = 0.0;
    double total_interest = 0.0;
    double closing_balance = 0.0;
    int flow_count = 0;
    std::optional<double> current_interest;
    InterestSource current_interest_source = InterestSource::Unavailable;
};

inline constexpr int kBucketCount = 19;

// Номер бакета 0..18 для даты. Границы считаются один раз от даты отчёта.
class Bucketizer {
public:
    explicit Bucketizer(Date report_date);
    int bucket(Date date) const;
    Date report_date() const { return report_date_; }

private:
    Date report_date_;
    Date upper_[kBucketCount - 2];
};

struct BucketKey {
    int32_t portfolio_key = 0;
    Currency currency = Currency::RUB;
    Side side = Side::Asset;

    auto operator<=>(const BucketKey&) const = default;
};

struct BucketTotals {
    double principal[kBucketCount] = {};
    double interest[kBucketCount] = {};
};

// Накопление потоков по ключу. map даёт одинаковый порядок вывода при любом запуске.
class Accumulator {
public:
    void add(const BucketKey& key, std::span<const CashFlow> flows, const Bucketizer& bucketizer);
    void merge(const Accumulator& other);
    std::map<BucketKey, BucketTotals> totals() const;

private:
    struct Sums {
        NeumaierSum principal[kBucketCount];
        NeumaierSum interest[kBucketCount];
    };
    std::map<BucketKey, Sums> sums_;
};

struct RunOptions {
    int threads = 1;
    bool keep_flows = false;
};

struct PortfolioResult {
    Accumulator accumulator;
    std::vector<CashFlow> flows;  // только при keep_flows
    std::vector<DealSummary> summaries;
    std::vector<DealError> errors;
};

ErrorCode validate(const Deal& deal, const DealArena& arena);

// Дополняет out потоками сделки. При ошибке out не меняется.
ErrorCode generate_cashflows(const Deal& deal, const DealArena& arena,
                             const Policy& policy, std::vector<CashFlow>& out,
                             DealSummary* summary = nullptr);

void aggregate(const Deal& deal, std::span<const CashFlow> flows,
               const Bucketizer& bucketizer, Accumulator& accumulator);

// Результат не зависит от числа потоков: состав блоков и порядок слияния фиксированы.
PortfolioResult run_portfolio(std::span<const Deal> deals, const DealArena& arena,
                              const Policy& policy, const RunOptions& options = {});

}  // namespace cfcore
