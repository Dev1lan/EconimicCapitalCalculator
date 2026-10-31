#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cf {

enum class DealType : uint8_t {
    SIMPLE,
    ANN,
    RGM
};

enum class RateType : uint8_t {
    FIXED,
    FLOAT
};

enum class DayCount : uint8_t {
    ACT_365,
    ACT_ACT_ISDA
};

// Элемент principal_payment
struct PrincipalStep {
    int32_t date{0};
    double amount{0.0};
};

// Элемент repricing_dttm_rate_schedule_fixed
struct RateStep {
    int32_t date{0};
    double rate{0.0};
};

struct ScheduleRule {
    std::optional<int32_t> start_date;
    std::optional<int32_t> period_months;
    std::optional<int32_t> period_days;
    std::optional<int32_t> day_num;
    bool is_end_date{false};

    bool has_rule() const noexcept {
        return is_end_date ||
               (start_date.has_value() &&
                (period_months.value_or(0) > 0 || period_days.value_or(0) > 0));
    }
};

struct DealInput {
    int64_t deal_id{0};
    int32_t report_date{0};
    int32_t deal_date{0};
    int32_t maturity_date{0};

    DealType discriminator{DealType::SIMPLE};
    RateType rate_type{RateType::FIXED};
    DayCount day_convention{DayCount::ACT_ACT_ISDA};

    double current_principal{0.0};
    double interest_rate{0.0};
    std::optional<double> base_rate;
    std::optional<double> cap_rate;
    std::optional<double> floor_rate;
    std::optional<double> accrued_interest;

    ScheduleRule regular_rule;
    ScheduleRule interest_rule;
    ScheduleRule repricing_rule;

    std::vector<int32_t> explicit_interest_dates;
    std::vector<PrincipalStep> explicit_principal_steps;
    std::vector<int32_t> explicit_repricing_dates;
    std::vector<RateStep> explicit_repricing_rates;
};

struct ScheduleEvent {
    int32_t date{0};
    bool is_interest{false};
    bool is_principal{false};
    bool is_repricing{false};
    std::optional<double> explicit_principal;
};

struct CashFlowRow {
    int64_t deal_id{0};
    int32_t cf_date{0};
    bool is_interest{false};
    bool is_principal{false};
    std::optional<double> csv_principal;
    int32_t period_start{0};
    double principal_before{0.0};
    double year_fraction{0.0};
    double cf_rate{0.0};
    double interest_accrued{0.0};
    double interest_accum_before{0.0};
    double interest_paid{0.0};
    double interest_accum_after{0.0};
    double principal_paid{0.0};
    double principal_after{0.0};
    double total_cf{0.0};
};


} // namespace cf