#include <algorithm>
#include <cmath>
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

// Общий цикл для всех типов сделок. Проценты доначисляются на каждом событии,
// поэтому погашение между процентными датами сразу меняет базу начисления.
ErrorCode settle(const Deal& deal, const DealArena& arena, const Policy& policy,
                 const std::vector<Event>& events, const internal::ScheduleInfo& info,
                 double start_rate, std::vector<CashFlow>& out, double& balance_out, EventTrace* trace) {
    const DayCount day_count = policy.force_act365 ? DayCount::Act365 : deal.day_count;
    const bool annuity = deal.amortization == AmortizationType::ANN;

    double balance = deal.outstanding_principal;
    double rate = start_rate;
    double component = info.rate_component;
    Date accrual_from = info.accrual_start;
    double accrued = 0.0;
    double accrued_fraction = 0.0;
    double last_interest = 0.0;
    int principal_left = info.principal_events;

    double payment = 0.0;
    if (annuity) {
        const auto report_rate = rate_on(deal, arena, policy, component, deal.report_date);
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

    for (const Event& event : events) {
        const Date period_start = accrual_from;
        const double balance_before = balance;
        const double rate_before = rate;
        const double fraction = year_fraction(accrual_from, event.date, day_count);
        const double interest_accrued = interest_amount(balance, rate, fraction);
        accrued += interest_accrued;
        accrued_fraction += fraction;
        accrual_from = std::max(accrual_from, event.date);

        const bool record = trace && (!trace->repricing_only || event.type == EventType::RateReset);
        RateSource source = RateSource::None;
        double interest_paid = 0.0;
        double principal_paid = 0.0;
        switch (event.type) {
            case EventType::InterestPayment:
                interest_paid = pay_interest(event.date);
                break;
            case EventType::PrincipalPayment: {
                --principal_left;
                double amount = event.amount ? *event.amount : payment - last_interest;
                if (principal_left == 0 && policy.close_residual) {
                    amount = balance;
                }
                principal_paid = apply_principal(balance, amount);
                if (principal_paid > 0.0) {
                    out.push_back({deal.instrument_id, event.date, FlowType::Principal, principal_paid, 0.0, 0.0});
                }
                break;
            }
            case EventType::RateReset: {
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
                                   interest_accrued, interest_paid, principal_paid});
        }
    }
    // ДОПУЩЕНИЕ: проценты после последней процентной даты выплачиваются с последним событием.
    const double final_interest = pay_interest(accrual_from);
    if (trace && !trace->repricing_only && final_interest > 0.0) {
        trace->rows.push_back({deal.instrument_id, accrual_from, EventType::InterestPayment,
                               accrual_from, accrual_from, 0.0, balance, balance, rate, rate,
                               RateSource::None, 0.0, final_interest, 0.0});
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
    if (!info.has_past_interest && deal.start_date > deal.report_date) {
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
    internal::ScheduleInfo info;
    if (ErrorCode code = internal::build_events(deal, arena, policy, events, info); code != ErrorCode::Ok) {
        return code;
    }
    const auto start_rate = rate_on(deal, arena, policy, info.rate_component, info.accrual_start);
    if (!start_rate) {
        return ErrorCode::MissingRate;
    }

    const size_t first = out.size();
    const size_t first_event = trace ? trace->rows.size() : 0;
    double closing_balance = 0.0;
    ErrorCode code = settle(deal, arena, policy, events, info, *start_rate, out, closing_balance, trace);

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
