#include <algorithm>
#include <cmath>
#include <vector>

#include "cfcore/engine.hpp"
#include "internal/schedule.hpp"

namespace cfcore {

namespace {

// Значение индикатора. До даты отчёта берём историю, дальше последнее известное значение.
std::optional<double> indicator_on(const Deal& deal, const DealArena& arena, Date date) {
    if ((date <= deal.report_date || !deal.indicator_rate) && !arena.indicator_fixings.empty()) {
        const Date known_on = std::min(date, deal.report_date);
        if (const auto value = last_known_rate(arena.indicator_fixings, known_on)) {
            return value;
        }
    }
    return deal.indicator_rate;
}

std::optional<double> rate_on(const Deal& deal, const DealArena& arena, const Policy& policy,
                              double component, Date date) {
    if (deal.rate_type == RateType::Fixed) {
        return effective_rate(component, deal.floor, deal.cap);
    }
    const std::optional<double> indicator = indicator_on(deal, arena, date);
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
                 double start_rate, std::vector<CashFlow>& out, double& balance_out) {
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
        if (accrued > 0.0) {
            out.push_back({deal.instrument_id, date, FlowType::Interest, accrued, rate, accrued_fraction});
        }
        last_interest = accrued;
        accrued = 0.0;
        accrued_fraction = 0.0;
    };

    for (const Event& event : events) {
        const double fraction = year_fraction(accrual_from, event.date, day_count);
        accrued += interest_amount(balance, rate, fraction);
        accrued_fraction += fraction;
        accrual_from = std::max(accrual_from, event.date);

        switch (event.type) {
            case EventType::InterestPayment:
                pay_interest(event.date);
                break;
            case EventType::PrincipalPayment: {
                --principal_left;
                double amount = event.amount ? *event.amount : payment - last_interest;
                if (principal_left == 0 && policy.close_residual) {
                    amount = balance;
                }
                const double paid = apply_principal(balance, amount);
                if (paid > 0.0) {
                    out.push_back({deal.instrument_id, event.date, FlowType::Principal, paid, 0.0, 0.0});
                }
                break;
            }
            case EventType::RateReset: {
                if (event.amount) {
                    component = *event.amount;
                }
                const auto new_rate = rate_on(deal, arena, policy, component, event.date);
                if (!new_rate) {
                    return ErrorCode::MissingRate;
                }
                // Новая ставка меняет размер оставшихся платежей аннуитета.
                if (annuity && *new_rate != rate && event.date > deal.report_date && principal_left > 0) {
                    payment = annuity_payment(balance, *new_rate, principal_left);
                }
                rate = *new_rate;
                break;
            }
        }
    }
    // ДОПУЩЕНИЕ: проценты после последней процентной даты выплачиваются с последним событием.
    pay_interest(accrual_from);
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
                             std::vector<CashFlow>& out, DealSummary* summary) {
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
    double closing_balance = 0.0;
    ErrorCode code = settle(deal, arena, policy, events, info, *start_rate, out, closing_balance);

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
