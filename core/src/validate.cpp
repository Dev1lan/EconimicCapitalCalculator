#include <cmath>
#include <span>

#include "cfcore/date.hpp"
#include "cfcore/engine.hpp"

namespace cfcore {

namespace {

// Коридор защищает от переполнений в расчёте дат и зависания цикла графика.
const Date kMinDate = from_ymd(1900, 1, 1);
const Date kMaxDate = from_ymd(2200, 1, 1);

bool date_in_range(Date date) {
    return date >= kMinDate && date <= kMaxDate;
}

bool optional_finite(const std::optional<double>& value) {
    return !value || std::isfinite(*value);
}

template <class T>
bool span_in_arena(const Span& span, const std::vector<T>& items) {
    return uint64_t{span.offset} + span.length <= items.size();
}

ErrorCode check_rule(const PaymentRule& rule, bool allow_last_day) {
    if (rule.kind == PaymentRule::Kind::None || rule.kind == PaymentRule::Kind::EndDate) {
        return ErrorCode::Ok;
    }
    if (rule.period < 1) {
        return ErrorCode::InvalidSchedule;
    }
    if (!date_in_range(rule.start)) {
        return ErrorCode::InvalidDate;
    }
    const bool last_day = allow_last_day && rule.day == -1;
    if (!last_day && (rule.day < 0 || rule.day > 31)) {
        return ErrorCode::InvalidSchedule;
    }
    return ErrorCode::Ok;
}

ErrorCode check_numbers(const Deal& deal) {
    if (!std::isfinite(deal.outstanding_principal) || deal.outstanding_principal <= 0.0) {
        return ErrorCode::InvalidNumber;
    }
    if (!std::isfinite(deal.rate) || !optional_finite(deal.indicator_rate) ||
        !optional_finite(deal.cap) || !optional_finite(deal.floor) ||
        !optional_finite(deal.accrued_interest)) {
        return ErrorCode::InvalidNumber;
    }
    if (deal.rate_type == RateType::Fixed && deal.rate < 0.0) {
        return ErrorCode::InvalidNumber;
    }
    if (deal.cap && deal.floor && *deal.floor > *deal.cap) {
        return ErrorCode::InvalidNumber;
    }
    return ErrorCode::Ok;
}

ErrorCode check_lists(const Deal& deal, const DealArena& arena) {
    if (!span_in_arena(deal.interest_schedule, arena.interest_dates) ||
        !span_in_arena(deal.principal_schedule, arena.principal_items) ||
        !span_in_arena(deal.reset_schedule, arena.reset_items)) {
        return ErrorCode::InvalidSchedule;
    }
    Date previous = kMinDate;
    for (const Date date : std::span(arena.interest_dates)
                               .subspan(deal.interest_schedule.offset, deal.interest_schedule.length)) {
        if (!date_in_range(date)) {
            return ErrorCode::InvalidDate;
        }
        if (date < previous) {
            return ErrorCode::InvalidSchedule;
        }
        previous = date;
    }
    previous = kMinDate;
    for (const PrincipalItem& item : std::span(arena.principal_items)
                                         .subspan(deal.principal_schedule.offset, deal.principal_schedule.length)) {
        if (!date_in_range(item.date)) {
            return ErrorCode::InvalidDate;
        }
        if (!std::isfinite(item.amount) || item.amount < 0.0) {
            return ErrorCode::InvalidNumber;
        }
        if (item.date < previous) {
            return ErrorCode::InvalidSchedule;
        }
        previous = item.date;
    }
    for (const ResetItem& item : std::span(arena.reset_items)
                                     .subspan(deal.reset_schedule.offset, deal.reset_schedule.length)) {
        if (!date_in_range(item.date)) {
            return ErrorCode::InvalidDate;
        }
        if (!optional_finite(item.fixed_rate)) {
            return ErrorCode::InvalidNumber;
        }
    }
    return ErrorCode::Ok;
}

}  // namespace

ErrorCode validate(const Deal& deal, const DealArena& arena) {
    if (ErrorCode code = check_numbers(deal); code != ErrorCode::Ok) {
        return code;
    }
    if (!date_in_range(deal.report_date) || !date_in_range(deal.start_date) ||
        !date_in_range(deal.maturity_date) || deal.maturity_date <= deal.report_date) {
        return ErrorCode::InvalidDate;
    }
    for (const PaymentRule* rule : {&deal.interest_rule, &deal.principal_rule}) {
        if (ErrorCode code = check_rule(*rule, false); code != ErrorCode::Ok) {
            return code;
        }
    }
    if (ErrorCode code = check_rule(deal.reset_rule, true); code != ErrorCode::Ok) {
        return code;
    }
    // Формула аннуитета рассчитана на ежемесячный платёж.
    if (deal.amortization == AmortizationType::ANN) {
        const PaymentRule& rule = deal.principal_rule.kind != PaymentRule::Kind::None
                                      ? deal.principal_rule
                                      : deal.interest_rule;
        const bool monthly = rule.kind == PaymentRule::Kind::Months && rule.period == 1;
        if (!monthly && rule.kind != PaymentRule::Kind::EndDate &&
            rule.kind != PaymentRule::Kind::None) {
            return ErrorCode::Unsupported;
        }
    }
    return check_lists(deal, arena);
}

}  // namespace cfcore
