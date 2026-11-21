#include "internal/schedule.hpp"

#include <algorithm>
#include <map>
#include <span>

#include "cfcore/date.hpp"

namespace cfcore::internal {

namespace {

// Допуск сверки суммы графика с остатком на один платёж: суммы в данных округлены до копеек
// (у сделки 44 график меньше остатка на 0,01).
constexpr double kScheduleToleranceRub = 0.01;

int rule_day(const PaymentRule& rule) {
    if (rule.day == -1) {
        return 31;  // add_months обрежет до последнего дня месяца
    }
    return rule.day != 0 ? rule.day : to_ymd(rule.start).day;
}

Date rule_date(const PaymentRule& rule, int step) {
    if (rule.kind == PaymentRule::Kind::Days) {
        return rule.start + step * rule.period;
    }
    return add_months(rule.start, step * rule.period, rule_day(rule));
}

// Номер шага, с которого начинается поиск последней даты не позже cutoff.
int first_guess_step(const PaymentRule& rule, Date cutoff) {
    if (rule.kind == PaymentRule::Kind::Days) {
        return (cutoff - rule.start) / rule.period;
    }
    const YearMonthDay a = to_ymd(rule.start);
    const YearMonthDay b = to_ymd(cutoff);
    return ((b.year - a.year) * 12 + b.month - a.month) / rule.period;
}

// Даты правила после cutoff и строго до end. previous: последняя дата не позже cutoff.
ErrorCode rule_dates(const PaymentRule& rule, Date cutoff, Date end,
                     std::vector<Date>& dates, Date& previous, bool& has_previous) {
    has_previous = false;
    if (rule.kind == PaymentRule::Kind::None || rule.kind == PaymentRule::Kind::EndDate) {
        return ErrorCode::Ok;
    }
    int step = first_guess_step(rule, cutoff);
    while (step >= 0 && rule_date(rule, step) > cutoff) {
        --step;
    }
    if (step >= 0) {
        previous = rule_date(rule, step);
        has_previous = true;
    } else {
        step = -1;
    }
    for (int j = step + 1;; ++j) {
        const Date date = rule_date(rule, j);
        if (date >= end) {
            return ErrorCode::Ok;
        }
        if (static_cast<int>(dates.size()) >= kMaxEvents) {
            return ErrorCode::TooManyEvents;
        }
        if (date > cutoff) {
            dates.push_back(date);
        }
    }
}

struct DateSet {
    std::vector<Date> dates;
    Date previous = 0;
    bool has_previous = false;
};

// Даты по правилу плюс платёж на дату погашения.
ErrorCode dates_with_maturity(const PaymentRule& rule, const Deal& deal, Date cutoff,
                              DateSet& set) {
    set.dates.clear();
    const ErrorCode code = rule_dates(rule, cutoff, deal.maturity_date, set.dates,
                                      set.previous, set.has_previous);
    if (code == ErrorCode::Ok) {
        set.dates.push_back(deal.maturity_date);
    }
    return code;
}

const PaymentRule& principal_source(const Deal& deal) {
    return deal.principal_rule.kind != PaymentRule::Kind::None ? deal.principal_rule
                                                               : deal.interest_rule;
}

ErrorCode interest_dates(const Deal& deal, const DealArena& arena, Date cutoff, DateSet& set) {
    if (deal.interest_schedule.length == 0) {
        const PaymentRule& rule = deal.interest_rule.kind != PaymentRule::Kind::None
                                      ? deal.interest_rule
                                      : deal.principal_rule;
        return dates_with_maturity(rule, deal, cutoff, set);
    }
    set.dates.clear();
    set.has_previous = false;
    const auto list = std::span(arena.interest_dates)
                          .subspan(deal.interest_schedule.offset, deal.interest_schedule.length);
    for (const Date date : list) {
        if (date > cutoff) {
            set.dates.push_back(date);
        } else {
            set.previous = date;
            set.has_previous = true;
        }
    }
    return ErrorCode::Ok;
}

void add_principal_events(std::span<const Date> dates, std::optional<double> amount,
                          std::vector<Event>& events, ScheduleInfo& info) {
    for (const Date date : dates) {
        events.push_back({date, EventType::PrincipalPayment, amount});
    }
    info.principal_events = static_cast<int>(dates.size());
}

// ДОПУЩЕНИЕ: SIMPLE без списка основного долга гасится разово на дату погашения.
ErrorCode add_simple_principal(const Deal& deal, const DealArena& arena, Date cutoff,
                               std::vector<Event>& events, ScheduleInfo& info) {
    const double balance = deal.outstanding_principal;
    if (deal.principal_schedule.length == 0) {
        events.push_back({deal.maturity_date, EventType::PrincipalPayment, balance});
        info.principal_events = 1;
        return ErrorCode::Ok;
    }
    const auto list = std::span(arena.principal_items)
                          .subspan(deal.principal_schedule.offset, deal.principal_schedule.length);
    double scheduled = 0.0;
    for (const PrincipalItem& item : list) {
        if (item.date > cutoff) {
            events.push_back({item.date, EventType::PrincipalPayment, item.amount});
            scheduled += item.amount;
            ++info.principal_events;
        }
    }
    const double tolerance = kScheduleToleranceRub * info.principal_events;
    if (scheduled > balance + tolerance) {
        return ErrorCode::ScheduleSumMismatch;
    }
    if (scheduled < balance - tolerance) {
        return ErrorCode::ScheduleEndsEarly;
    }
    return ErrorCode::Ok;
}

ErrorCode add_reset_events(const Deal& deal, const DealArena& arena, std::vector<Event>& events,
                           ScheduleInfo& info) {
    const auto list = std::span(arena.reset_items)
                          .subspan(deal.reset_schedule.offset, deal.reset_schedule.length);
    for (const ResetItem& item : list) {
        if (item.date > info.accrual_start && item.date < deal.maturity_date) {
            events.push_back({item.date, EventType::RateReset, item.fixed_rate});
        } else if (item.date <= info.accrual_start && item.fixed_rate) {
            info.rate_component = *item.fixed_rate;
        }
    }
    std::vector<Date> dates;
    Date previous = 0;
    bool has_previous = false;
    const ErrorCode code = rule_dates(deal.reset_rule, info.accrual_start, deal.maturity_date,
                                      dates, previous, has_previous);
    for (const Date date : dates) {
        events.push_back({date, EventType::RateReset, std::nullopt});
    }
    return code;
}

ErrorCode add_contractual_resets(const Deal& deal, const DealArena& arena,
                                 std::vector<Event>& events, ScheduleInfo& info) {
    std::map<Date, std::optional<double>> resets;
    for (const bool is_final : {false, true}) {
        const Span span = is_final ? deal.final_rate_schedule : deal.reset_schedule;
        for (const auto& item : std::span(arena.reset_items).subspan(span.offset, span.length)) {
            if (deal.rate_type == RateType::Float && !is_final && item.fixed_rate) {
                return ErrorCode::AmbiguousRateSchedule;
            }
            if (item.date < deal.start_date || item.date > deal.maturity_date) {
                if (is_final) {
                    return ErrorCode::InvalidSchedule;
                }
                continue;
            }
            auto& value = resets[item.date];
            if (value && item.fixed_rate && *value != *item.fixed_rate) {
                return ErrorCode::ConflictingRepricingRates;
            }
            if (item.fixed_rate) {
                value = item.fixed_rate;
            }
        }
    }

    std::vector<Date> dates;
    Date previous = 0;
    bool has_previous = false;
    const auto code = rule_dates(deal.reset_rule, info.accrual_start, deal.maturity_date,
                                 dates, previous, has_previous);
    if (code != ErrorCode::Ok) {
        return code;
    }
    if (has_previous && previous >= deal.start_date && deal.rate_type == RateType::Float) {
        resets.try_emplace(previous);
    }
    for (const Date date : dates) {
        resets.try_emplace(date);
    }
    for (const auto& [date, rate] : resets) {
        if (date == deal.maturity_date) {
            break;
        }
        if (date > info.accrual_start) {
            events.push_back({date, EventType::RateReset, rate});
        } else if (rate || deal.rate_type == RateType::Float) {
            info.initial_reset = ResetItem{date, rate};
        }
    }
    return ErrorCode::Ok;
}

}  // namespace

ErrorCode next_planned_reset(const Deal& deal, const DealArena& arena, std::optional<Date>& next) {
    next.reset();
    const auto consider = [&](Date date) {
        if (date > deal.report_date && (!next || date < *next)) {
            next = date;
        }
    };
    for (const Span span : {deal.reset_schedule, deal.final_rate_schedule}) {
        for (const auto& item : std::span(arena.reset_items).subspan(span.offset, span.length)) {
            if (item.date < deal.start_date || item.date > deal.maturity_date) {
                next.reset();
                return ErrorCode::InvalidSchedule;
            }
            consider(item.date);
        }
    }
    if (deal.reset_rule.kind == PaymentRule::Kind::EndDate) {
        consider(deal.maturity_date);
    }
    std::vector<Date> dates;
    Date previous = 0;
    bool has_previous = false;
    const Date cutoff = std::max(deal.report_date, deal.start_date - 1);
    const auto code = rule_dates(deal.reset_rule, cutoff, deal.maturity_date + 1,
                                 dates, previous, has_previous);
    if (code != ErrorCode::Ok) {
        next.reset();
        return code;
    }
    if (!dates.empty()) {
        consider(dates.front());
    }
    return ErrorCode::Ok;
}

ErrorCode build_events(const Deal& deal, const DealArena& arena, const Policy& policy,
                       std::vector<Event>& events, ScheduleInfo& info) {
    static thread_local DateSet interest;
    static thread_local DateSet principal;

    events.clear();
    info = ScheduleInfo{};
    info.rate_component = deal.rate;

    const Date cutoff = policy.include_payment_on_report ? deal.report_date - 1 : deal.report_date;

    ErrorCode code = ErrorCode::Ok;
    if (deal.amortization == AmortizationType::SIMPLE) {
        code = add_simple_principal(deal, arena, cutoff, events, info);
    } else {
        code = dates_with_maturity(principal_source(deal), deal, cutoff, principal);
        if (code == ErrorCode::Ok) {
            // В ANN сумма зависит от процентов и считается в цикле.
            std::optional<double> amount;
            if (deal.amortization == AmortizationType::RGM) {
                amount = deal.outstanding_principal / static_cast<double>(principal.dates.size());
            }
            add_principal_events(principal.dates, amount, events, info);
        }
    }
    if (code != ErrorCode::Ok) {
        return code;
    }

    // ANN: один платёж включает проценты и долг, поэтому даты общие.
    code = deal.amortization == AmortizationType::ANN
               ? dates_with_maturity(principal_source(deal), deal, cutoff, interest)
               : interest_dates(deal, arena, cutoff, interest);
    if (code != ErrorCode::Ok) {
        return code;
    }
    for (const Date date : interest.dates) {
        events.push_back({date, EventType::InterestPayment, std::nullopt});
    }

    // ДОПУЩЕНИЕ: если прошлых дат нет, проценты идут от начала сделки.
    const Date previous = interest.has_previous
                              ? std::max(interest.previous, deal.start_date)
                              : deal.start_date;
    info.has_past_interest = interest.has_previous;
    info.last_interest_date = previous;
    info.accrual_start = !policy.is_contractual_v2() && policy.first_period_from_prev_pay
                             ? previous
                             : std::max(deal.report_date, deal.start_date);

    code = policy.is_contractual_v2() ? add_contractual_resets(deal, arena, events, info)
                                      : add_reset_events(deal, arena, events, info);
    if (code != ErrorCode::Ok) {
        return code;
    }
    if (static_cast<int>(events.size()) > kMaxEvents) {
        return ErrorCode::TooManyEvents;
    }
    std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
        return a.date != b.date ? a.date < b.date : a.type < b.type;
    });
    return ErrorCode::Ok;
}

}  // namespace cfcore::internal
