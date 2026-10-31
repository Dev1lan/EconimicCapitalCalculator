#pragma once

#include "cf_types.hpp"
#include <vector>

namespace cf {

struct CalculatorConfig {
    double default_key_rate{0.08};
    double default_float_cap{1.0};
    double default_float_floor{-1.0};
};

std::vector<CashFlowRow> calculate_cash_flows(
    const DealInput& deal,
    const CalculatorConfig& config = CalculatorConfig{}
);

double resolve_cf_rate(
    const DealInput& deal,
    int32_t period_start,
    int32_t period_end,
    const CalculatorConfig& config = CalculatorConfig{}
) noexcept;

std::vector<CashFlowRow> calculate_simple(
    const DealInput& deal,
    const std::vector<ScheduleEvent>& timeline,
    const CalculatorConfig& config
);

std::vector<CashFlowRow> calculate_ann(
    const DealInput& deal,
    const std::vector<ScheduleEvent>& timeline,
    const CalculatorConfig& config
);

std::vector<CashFlowRow> calculate_rgm(
    const DealInput& deal,
    const std::vector<ScheduleEvent>& timeline,
    const CalculatorConfig& config
);

} // namespace cf
