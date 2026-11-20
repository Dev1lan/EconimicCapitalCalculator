#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "cfcore/engine.hpp"
#include "internal/schedule.hpp"

namespace cfcore {

namespace {

// Значение индикатора. До даты отчёта берём историю, дальше последнее известное значение.
std::optional<double> indicator_on(const Deal& deal, const DealArena& arena, Date date,
                                   RateSource* source) {
    if ((date <= deal.report_date || !deal.indicator_rate) && !arena.indicator_fixings.empty()) {
        const Date known_on = std::min(date, deal.report_date);
        if (const auto value = last_known_rate(arena.indicator_fixings, known_on)) {
            if (source) {
                *source = RateSource::FixingHistory;
            }
            return value;
        }
    }
    if (source && deal.indicator_rate) {
        *source = RateSource::DealIndicator;
    }
    return deal.indicator_rate;
}

std::optional<double> rate_on(const Deal& deal, const DealArena& arena, const Policy& policy,
                              double component, Date date, RateSource* source = nullptr) {
    if (deal.rate_type == RateType::Fixed) {
        if (source) {
            *source = RateSource::ContractRate;
        }
        return effective_rate(component, deal.floor, deal.cap);
    }
    const std::optional<double> indicator = indicator_on(deal, arena, date, source);
    if (!indicator) {
        return std::nullopt;
    }
    const double rate = policy.float_rate_is_spread ? *indicator + component : *indicator;
    return effective_rate(rate, deal.floor, deal.cap);
}

std::optional<double> contractual_rate_on(const Deal& deal, const DealArena& arena, Date reset_date,
                                          std::optional<double> final_rate, RateSource& source) {
    if (final_rate) {
        source = RateSource::ContractSchedule;
        return effective_rate(*final_rate, deal.floor, deal.cap);
    }
    if (deal.rate_type == RateType::Fixed) {
        source = RateSource::ContractRate;
        return effective_rate(deal.rate, deal.floor, deal.cap);
    }

    const auto lag = deal.indicator_lag_days.value_or(0);
    const int64_t fixing_date = static_cast<int64_t>(reset_date) - lag;
    if (arena.external_fixings || !arena.indicator_fixings.empty()) {
        if (fixing_date < std::numeric_limits<Date>::min()) {
            return std::nullopt;
        }
        const Date known_on = std::min(static_cast<Date>(fixing_date), deal.report_date);
        if (const auto fixing = last_known_rate(arena.indicator_fixings, known_on)) {
            source = RateSource::FixingHistory;
            return effective_rate(*fixing + deal.rate, deal.floor, deal.cap);
        }
    } else if (lag == 0 && deal.indicator_rate) {
        source = RateSource::DealIndicator;
        return effective_rate(*deal.indicator_rate + deal.rate, deal.floor, deal.cap);
    }
    return std::nullopt;
}

// Общий цикл для всех типов сделок. Проценты доначисляются на каждом событии,
// поэтому погашение между процентными датами сразу меняет базу начисления.
ErrorCode settle(const Deal& deal, const DealArena& arena, const Policy& policy,
                 const std::vector<Event>& events, const internal::ScheduleInfo& info,
                 double start_rate, RateSource active_source, std::vector<CashFlow>& out,
                 double& balance_out, EventTrace* trace) {
    const DayCount day_count = policy.force_act365 ? DayCount::Act365 : deal.day_count;
    const bool annuity = deal.amortization == AmortizationType::ANN;

    double balance = deal.outstanding_principal;
    double rate = start_rate;
    double component = info.rate_component;
    Date accrual_from = info.accrual_start;
    const bool contractual = policy.is_contractual_v2();
    const bool deferred_annuity = contractual && annuity && deal.rate_type == RateType::Float;
    std::optional<Date> active_reset;
    if (info.initial_reset) {
        active_reset = info.initial_reset->date;
    }

    auto opening_interest = contractual ? deal.accrued_interest : std::nullopt;
    double accrued = opening_interest.value_or(0.0);
    double accrued_fraction = 0.0;
    double last_interest = 0.0;
    int principal_left = info.principal_events;

    double payment = 0.0;
    double payment_rate = start_rate;
    std::optional<Date> last_principal_date;
    const auto reprice_annuity = [&] {
        payment = annuity_payment(balance, rate, principal_left);
        payment_rate = rate;
    };
    if (annuity) {
        const auto report_rate = contractual ? std::optional<double>(start_rate)
                                             : rate_on(deal, arena, policy, component, deal.report_date);
        if (!report_rate) {
            return ErrorCode::MissingRate;
        }
        payment = annuity_payment(balance, *report_rate, principal_left);
    }

    const auto pay_interest = [&](Date date) {
        const double paid = accrued > 0.0 ? accrued : 0.0;
        if (paid > 0.0) {
            out.push_back({deal.instrument_id, date, FlowType::Interest, paid, rate, accrued_fraction});
        }
        last_interest = accrued;
        accrued = 0.0;
        accrued_fraction = 0.0;
        return paid;
    };

    for (size_t i = 0; i < events.size(); ++i) {
        const Event& event = events[i];
        const Date period_start = accrual_from;
        const double balance_before = balance;
        const double rate_before = rate;
        const auto source_before = active_source;
        const auto reset_before = active_reset;
        const double pmt_before = payment;
        const double fraction = year_fraction(accrual_from, event.date, day_count);
        const double interest_accrued = interest_amount(balance, rate, fraction);
        accrued += interest_accrued;
        accrued_fraction += fraction;
        accrual_from = std::max(accrual_from, event.date);

        const bool record = trace && (!trace->repricing_only || event.type == EventType::RateReset);
        RateSource source = RateSource::None;
        double interest_paid = 0.0;
        double principal_paid = 0.0;
        std::optional<double> included;
        switch (event.type) {
            case EventType::InterestPayment:
                included = opening_interest;
                opening_interest.reset();
                interest_paid = pay_interest(event.date);
                break;
            case EventType::PrincipalPayment: {
                if (deferred_annuity && last_interest - payment > 1e-8) {
                    return ErrorCode::InterestExceedsAnnuity;
                }
                --principal_left;
                double amount = event.amount ? *event.amount : payment - last_interest;
                if (principal_left == 0 && policy.close_residual) {
                    amount = balance;
                }
                principal_paid = apply_principal(balance, amount);
                if (principal_paid > 0.0) {
                    out.push_back({deal.instrument_id, event.date, FlowType::Principal, principal_paid, 0.0, 0.0});
                }
                if (deferred_annuity) {
                    last_principal_date = event.date;
                    const bool reset_today = i + 1 < events.size() && events[i + 1].date == event.date &&
                                             events[i + 1].type == EventType::RateReset;
                    if (principal_left > 0 && rate != payment_rate && !reset_today) {
                        reprice_annuity();
                    }
                }
                break;
            }
            case EventType::RateReset: {
                if (contractual) {
                    const bool retain_fixed = deal.rate_type == RateType::Fixed && !event.amount;
                    source = active_source;
                    const auto new_rate = retain_fixed ? std::optional<double>(rate)
                        : contractual_rate_on(deal, arena, event.date, event.amount, source);
                    if (!new_rate) {
                        return ErrorCode::MissingRate;
                    }
                    rate = *new_rate;
                    active_source = source;
                    active_reset = event.date;
                    if (annuity && principal_left > 0 && rate != payment_rate &&
                        (!deferred_annuity || last_principal_date == event.date)) {
                        reprice_annuity();
                    }
                    break;
                }
                if (event.amount) {
                    component = *event.amount;
                }
                const auto new_rate = rate_on(deal, arena, policy, component, event.date,
                                              record ? &source : nullptr);
                if (!new_rate) {
                    return ErrorCode::MissingRate;
                }
                if (record && event.amount &&
                    (deal.rate_type == RateType::Fixed || policy.float_rate_is_spread)) {
                    source = RateSource::ContractSchedule;
                }
                // Новая ставка меняет размер оставшихся платежей аннуитета.
                if (annuity && *new_rate != rate && event.date > deal.report_date && principal_left > 0) {
                    payment = annuity_payment(balance, *new_rate, principal_left);
                }
                rate = *new_rate;
                break;
            }
        }
        if (record) {
            trace->rows.push_back({deal.instrument_id, event.date, event.type, period_start, accrual_from,
                                   fraction, balance_before, balance, rate_before, rate, source,
                                   interest_accrued, interest_paid, principal_paid,
                                   source_before, reset_before, included,
                                   annuity ? std::optional<double>(pmt_before) : std::nullopt,
                                   annuity ? std::optional<double>(payment) : std::nullopt});
        }
    }
    // ДОПУЩЕНИЕ: проценты после последней процентной даты выплачиваются с последним событием.
    const double final_interest = pay_interest(accrual_from);
    if (trace && !trace->repricing_only && final_interest > 0.0) {
        trace->rows.push_back({deal.instrument_id, accrual_from, EventType::InterestPayment,
                               accrual_from, accrual_from, 0.0, balance, balance, rate, rate,
                               RateSource::None, 0.0, final_interest, 0.0,
                               active_source, active_reset, opening_interest,
                               annuity ? std::optional<double>(payment) : std::nullopt,
                               annuity ? std::optional<double>(payment) : std::nullopt});
    }
    balance_out = balance;
    return ErrorCode::Ok;
}

void fill_current_interest(const Deal& deal, const internal::ScheduleInfo& info, double start_rate,
                           const Policy& policy, DealSummary& summary) {
    if (deal.accrued_interest) {
        summary.current_interest = deal.accrued_interest;
        summary.current_interest_source = InterestSource::Reported;
        return;
    }
    if (policy.is_contractual_v2() || (!info.has_past_interest && deal.start_date > deal.report_date)) {
        return;
    }
    const DayCount day_count = policy.force_act365 ? DayCount::Act365 : deal.day_count;
    const double fraction = year_fraction(info.last_interest_date, deal.report_date, day_count);
    summary.current_interest = interest_amount(deal.outstanding_principal, start_rate, fraction);
    summary.current_interest_source = InterestSource::Computed;
}

}  // namespace

ErrorCode generate_cashflows(const Deal& deal, const DealArena& arena, const Policy& policy,
                             std::vector<CashFlow>& out, DealSummary* summary, EventTrace* trace) {
    static thread_local std::vector<Event> events;

    if (ErrorCode code = validate(deal, arena); code != ErrorCode::Ok) {
        return code;
    }
    if (policy.is_contractual_v2()) {
        if (deal.accrued_interest.value_or(0.0) < 0.0) {
            return ErrorCode::InvalidNumber;
        }
        if (deal.unsupported_indicator_lag) {
            return ErrorCode::UnsupportedIndicatorLag;
        }
        if (deal.indicator_lag_days.value_or(0) < 0 ||
            (deal.rate_type == RateType::Fixed && deal.indicator_lag_days.value_or(0) != 0)) {
            return ErrorCode::InvalidIndicatorLag;
        }
    } else if (deal.final_rate_schedule.length != 0 || deal.indicator_lag_days) {
        return ErrorCode::Unsupported;
    }

    internal::ScheduleInfo info;
    if (ErrorCode code = internal::build_events(deal, arena, policy, events, info); code != ErrorCode::Ok) {
        return code;
    }
    RateSource source = RateSource::None;
    const auto start_rate = policy.is_contractual_v2()
        ? contractual_rate_on(deal, arena, info.initial_reset ? info.initial_reset->date : deal.start_date,
                             info.initial_reset ? info.initial_reset->fixed_rate : std::nullopt, source)
        : rate_on(deal, arena, policy, info.rate_component, info.accrual_start);
    if (!start_rate) {
        return ErrorCode::MissingRate;
    }

    const size_t first = out.size();
    const size_t first_event = trace ? trace->rows.size() : 0;
    double closing_balance = 0.0;
    ErrorCode code = settle(deal, arena, policy, events, info, *start_rate, source, out, closing_balance, trace);

    double total_principal = 0.0;
    double total_interest = 0.0;
    for (size_t i = first; code == ErrorCode::Ok && i < out.size(); ++i) {
        if (!std::isfinite(out[i].amount)) {
            code = ErrorCode::NonFiniteResult;
        } else if (out[i].type == FlowType::Principal) {
            total_principal += out[i].amount;
        } else {
            total_interest += out[i].amount;
        }
    }
    if (code != ErrorCode::Ok) {
        out.resize(first);
        if (trace) {
            trace->rows.resize(first_event);
        }
        return code;
    }
    if (summary != nullptr) {
        *summary = DealSummary{};
        summary->instrument_id = deal.instrument_id;
        summary->opening_principal = deal.outstanding_principal;
        summary->total_principal = total_principal;
        summary->total_interest = total_interest;
        summary->closing_balance = closing_balance;
        summary->flow_count = static_cast<int>(out.size() - first);
        fill_current_interest(deal, info, *start_rate, policy, *summary);
    }
    return ErrorCode::Ok;
}

}  // namespace cfcore
