#include "cf_calculator.hpp"
#include "date_utils.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace cf {

namespace {

std::vector<int32_t> build_interest_dates(const DealInput& deal) {
    // Если задан явный список дат купонов - без автоматического добавления купона в maturity_date
    if (!deal.explicit_interest_dates.empty()) {
        std::vector<int32_t> dates;
        dates.reserve(deal.explicit_interest_dates.size());
        for (int32_t d : deal.explicit_interest_dates) {
            if (d > deal.report_date && d <= deal.maturity_date) {
                dates.push_back(d);
            }
        }
        std::sort(dates.begin(), dates.end());
        dates.erase(std::unique(dates.begin(), dates.end()), dates.end());
        return dates;
    }

    // Если правило для процентов задано явно - генерируем по нему (с закрытием в maturity_date)
    if (deal.interest_rule.has_rule()) {
        return dates::generate_rule_dates(
            deal.interest_rule, deal.report_date, deal.maturity_date, /*include_maturity=*/true
        );
    }

    // Иначе синхронизируем даты процентов с регулярным правилом погашения
    if (deal.regular_rule.has_rule()) {
        return dates::generate_rule_dates(
            deal.regular_rule, deal.report_date, deal.maturity_date, /*include_maturity=*/true
        );
    }

    // Выплата в дату окончательного погашения по умолчанию
    if (deal.maturity_date > deal.report_date) {
        return {deal.maturity_date};
    }
    return {};
}

std::vector<int32_t> build_principal_dates(const DealInput& deal) {
    if (!deal.explicit_principal_steps.empty()) {
        // Даты напрямую из explicit_principal_steps в build_merged_timeline
        return {};
    }

    if (deal.discriminator == DealType::SIMPLE) {
        if (deal.maturity_date > deal.report_date) {
            return {deal.maturity_date};
        }
        return {};
    }

    if (deal.regular_rule.has_rule()) {
        return dates::generate_rule_dates(
            deal.regular_rule, deal.report_date, deal.maturity_date, /*include_maturity=*/true
        );
    }

    if (deal.interest_rule.has_rule()) {
        return dates::generate_rule_dates(
            deal.interest_rule, deal.report_date, deal.maturity_date, /*include_maturity=*/true
        );
    }

    if (deal.maturity_date > deal.report_date) {
        return {deal.maturity_date};
    }
    return {};
}

std::vector<int32_t> build_repricing_dates(const DealInput& deal) {
    if (deal.rate_type != RateType::FLOAT) {
        return {};
    }

    std::vector<int32_t> dates;

    if (!deal.explicit_repricing_dates.empty()) {
        for (int32_t d : deal.explicit_repricing_dates) {
            if (d > deal.report_date && d <= deal.maturity_date) {
                dates.push_back(d);
            }
        }
    } else if (!deal.explicit_repricing_rates.empty()) {
        for (const auto& step : deal.explicit_repricing_rates) {
            if (step.date > deal.report_date && step.date <= deal.maturity_date) {
                dates.push_back(step.date);
            }
        }
    } else if (deal.repricing_rule.has_rule()) {
        dates = dates::generate_rule_dates(
            deal.repricing_rule, deal.report_date, deal.maturity_date, /*include_maturity=*/false
        );
    }

    std::sort(dates.begin(), dates.end());
    dates.erase(std::unique(dates.begin(), dates.end()), dates.end());
    return dates;
}

} // namespace

double resolve_cf_rate(
    const DealInput& deal,
    int32_t period_start,
    int32_t /*period_end*/,
    const CalculatorConfig& config
) noexcept {
    if (deal.rate_type == RateType::FIXED) {
        return deal.interest_rate;
    }

    double raw_rate = 0.0;
    bool found_explicit_rate = false;

    if (!deal.explicit_repricing_rates.empty()) {
        for (const auto& step : deal.explicit_repricing_rates) {
            if (step.date <= period_start) {
                raw_rate = step.rate;
                found_explicit_rate = true;
            } else {
                break;
            }
        }
    }

    if (!found_explicit_rate) {
        double base = config.default_key_rate;
        if (deal.base_rate.has_value() && std::abs(*deal.base_rate) > 1e-12) {
            base = *deal.base_rate;
        }
        raw_rate = base + deal.interest_rate;
    }

    const double cap = deal.cap_rate.value_or(config.default_float_cap);
    const double floor = deal.floor_rate.value_or(config.default_float_floor);

    return std::clamp(raw_rate, floor, cap);
}

std::vector<CashFlowRow> calculate_simple(
    const DealInput& deal,
    const std::vector<ScheduleEvent>& timeline,
    const CalculatorConfig& config
) {
    std::vector<CashFlowRow> rows;
    rows.reserve(timeline.size());

    const bool has_explicit_principal = !deal.explicit_principal_steps.empty();
    const double sign = (deal.current_principal < 0.0) ? -1.0 : 1.0;

    int32_t prev_date = deal.report_date;
    double current_debt = deal.current_principal;
    double accum_interest = 0.0;

    for (const auto& ev : timeline) {
        CashFlowRow row{};
        row.deal_id = deal.deal_id;
        row.cf_date = ev.date;
        row.is_interest = ev.is_interest;
        row.is_principal = ev.is_principal;
        row.period_start = prev_date;
        row.principal_before = current_debt;
        row.year_fraction = dates::year_fraction(prev_date, ev.date, deal.day_convention);
        row.cf_rate = resolve_cf_rate(deal, prev_date, ev.date, config);

        row.interest_accrued = row.principal_before * row.cf_rate * row.year_fraction;
        row.interest_accum_before = accum_interest;

        if (ev.is_interest) {
            row.interest_paid = row.interest_accum_before + row.interest_accrued;
            row.interest_accum_after = 0.0;
        } else {
            row.interest_paid = 0.0;
            row.interest_accum_after = row.interest_accum_before + row.interest_accrued;
        }

        if (ev.is_principal) {
            if (has_explicit_principal && ev.explicit_principal.has_value()) {
                const double step_amt = std::abs(*ev.explicit_principal) * sign;
                row.csv_principal = step_amt;
                row.principal_paid = step_amt;
            } else {
                row.principal_paid = row.principal_before;
            }
        } else {
            row.principal_paid = 0.0;
        }

        row.principal_after = row.principal_before - row.principal_paid;
        row.total_cf = row.interest_paid + row.principal_paid;

        rows.push_back(row);

        prev_date = ev.date;
        current_debt = row.principal_after;
        accum_interest = row.interest_accum_after;
    }

    return rows;
}

std::vector<CashFlowRow> calculate_rgm(
    const DealInput& deal,
    const std::vector<ScheduleEvent>& timeline,
    const CalculatorConfig& config
) {
    std::vector<CashFlowRow> rows;
    rows.reserve(timeline.size());

    size_t principal_events_count = 0;
    for (const auto& ev : timeline) {
        if (ev.is_principal) {
            ++principal_events_count;
        }
    }

    const double equal_principal_share =
        (principal_events_count > 0)
            ? (deal.current_principal / static_cast<double>(principal_events_count))
            : 0.0;

    int32_t prev_date = deal.report_date;
    double current_debt = deal.current_principal;
    double accum_interest = 0.0;
    size_t principal_step_idx = 0;

    for (const auto& ev : timeline) {
        CashFlowRow row{};
        row.deal_id = deal.deal_id;
        row.cf_date = ev.date;
        row.is_interest = ev.is_interest;
        row.is_principal = ev.is_principal;
        row.period_start = prev_date;
        row.principal_before = current_debt;
        row.year_fraction = dates::year_fraction(prev_date, ev.date, deal.day_convention);
        row.cf_rate = resolve_cf_rate(deal, prev_date, ev.date, config);

        row.interest_accrued = row.principal_before * row.cf_rate * row.year_fraction;
        row.interest_accum_before = accum_interest;

        if (ev.is_interest) {
            row.interest_paid = row.interest_accum_before + row.interest_accrued;
            row.interest_accum_after = 0.0;
        } else {
            row.interest_paid = 0.0;
            row.interest_accum_after = row.interest_accum_before + row.interest_accrued;
        }

        if (ev.is_principal) {
            ++principal_step_idx;
            if (principal_step_idx == principal_events_count) {
                row.principal_paid = row.principal_before;
                row.principal_after = 0.0;
            } else {
                row.principal_paid = equal_principal_share;
                row.principal_after = row.principal_before - row.principal_paid;
            }
        } else {
            row.principal_paid = 0.0;
            row.principal_after = row.principal_before;
        }

        row.total_cf = row.interest_paid + row.principal_paid;
        rows.push_back(row);

        prev_date = ev.date;
        current_debt = row.principal_after;
        accum_interest = row.interest_accum_after;
    }

    return rows;
}

std::vector<CashFlowRow> calculate_ann(
    const DealInput& deal,
    const std::vector<ScheduleEvent>& timeline,
    const CalculatorConfig& config
) {
    std::vector<CashFlowRow> rows;
    if (timeline.empty()) {
        return rows;
    }
    rows.reserve(timeline.size());

    int32_t period_months = 1;
    if (deal.regular_rule.period_months.value_or(0) > 0) {
        period_months = *deal.regular_rule.period_months;
    } else if (deal.interest_rule.period_months.value_or(0) > 0) {
        period_months = *deal.interest_rule.period_months;
    }

    auto compute_standard_pmt = [&](size_t start_idx, double remaining_principal, double annual_rate) -> double {
        size_t n_payments = 0;
        for (size_t k = start_idx; k < timeline.size(); ++k) {
            if (timeline[k].is_principal) {
                ++n_payments;
            }
        }
        if (n_payments == 0) {
            return 0.0;
        }

        const double i_rate = annual_rate * (static_cast<double>(period_months) / 12.0);
        if (std::abs(i_rate) < 1e-12) {
            return remaining_principal / static_cast<double>(n_payments);
        }

        const double factor = std::pow(1.0 + i_rate, static_cast<double>(n_payments));
        return remaining_principal * (i_rate * factor) / (factor - 1.0);
    };

    int32_t prev_date = deal.report_date;
    double current_debt = deal.current_principal;
    double accum_interest = 0.0;
    double prev_rate = resolve_cf_rate(deal, deal.report_date, timeline.front().date, config);
    double pmt = compute_standard_pmt(0, deal.current_principal, prev_rate);

    size_t last_principal_idx = 0;
    for (size_t i = 0; i < timeline.size(); ++i) {
        if (timeline[i].is_principal) {
            last_principal_idx = i;
        }
    }

    for (size_t i = 0; i < timeline.size(); ++i) {
        const auto& ev = timeline[i];
        const double current_rate = resolve_cf_rate(deal, prev_date, ev.date, config);

        if (i > 0 && std::abs(current_rate - prev_rate) > 1e-12) {
            pmt = compute_standard_pmt(i, current_debt, current_rate);
        }
        prev_rate = current_rate;

        CashFlowRow row{};
        row.deal_id = deal.deal_id;
        row.cf_date = ev.date;
        row.is_interest = ev.is_interest;
        row.is_principal = ev.is_principal;
        row.period_start = prev_date;
        row.principal_before = current_debt;
        row.year_fraction = dates::year_fraction(prev_date, ev.date, deal.day_convention);
        row.cf_rate = current_rate;

        row.interest_accrued = row.principal_before * row.cf_rate * row.year_fraction;
        row.interest_accum_before = accum_interest;

        if (ev.is_interest) {
            row.interest_paid = row.interest_accum_before + row.interest_accrued;
            row.interest_accum_after = 0.0;
        } else {
            row.interest_paid = 0.0;
            row.interest_accum_after = row.interest_accum_before + row.interest_accrued;
        }

        if (ev.is_principal) {
            if (i == last_principal_idx) {
                row.principal_paid = row.principal_before;
                row.principal_after = 0.0;
                row.total_cf = row.interest_paid + row.principal_paid;
            } else {
                row.total_cf = pmt;
                row.principal_paid = pmt - row.interest_paid;
                row.principal_after = row.principal_before - row.principal_paid;
            }
        } else {
            row.principal_paid = 0.0;
            row.principal_after = row.principal_before;
            row.total_cf = row.interest_paid;
        }

        rows.push_back(row);

        prev_date = ev.date;
        current_debt = row.principal_after;
        accum_interest = row.interest_accum_after;
    }

    return rows;
}

std::vector<CashFlowRow> calculate_cash_flows(
    const DealInput& deal,
    const CalculatorConfig& config
) {
    if (deal.maturity_date <= deal.report_date) {
        return {};
    }

    const std::vector<int32_t> interest_dates = build_interest_dates(deal);
    const std::vector<int32_t> principal_dates = build_principal_dates(deal);
    const std::vector<int32_t> repricing_dates = build_repricing_dates(deal);

    const std::vector<ScheduleEvent> timeline = dates::build_merged_timeline(
        interest_dates,
        principal_dates,
        deal.explicit_principal_steps,
        repricing_dates,
        deal.report_date,
        deal.maturity_date
    );

    switch (deal.discriminator) {
        case DealType::SIMPLE:
            return calculate_simple(deal, timeline, config);
        case DealType::ANN:
            return calculate_ann(deal, timeline, config);
        case DealType::RGM:
            return calculate_rgm(deal, timeline, config);
    }

    throw std::invalid_argument("Unsupported DealType discriminator");
}

} // namespace cf
