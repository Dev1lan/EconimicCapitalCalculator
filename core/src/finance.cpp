#include "cfcore/finance.hpp"

#include <algorithm>
#include <iterator>

#include "cfcore/date.hpp"

namespace cfcore {

namespace {

double act_act_isda(Date from, Date to) {
    double fraction = 0.0;
    int year = to_ymd(from).year;
    Date period_start = from;
    while (period_start < to) {
        const Date next_year = from_ymd(year + 1, 1, 1);
        const Date period_end = std::min(to, next_year);
        const double days_in_year = is_leap_year(year) ? 366.0 : 365.0;
        fraction += (period_end - period_start) / days_in_year;
        period_start = period_end;
        ++year;
    }
    return fraction;
}

}  // namespace

double year_fraction(Date from, Date to, DayCount day_count) {
    if (to <= from) {
        return 0.0;
    }
    switch (day_count) {
        case DayCount::Act365:
            return (to - from) / 365.0;
        case DayCount::ActActIsda:
            return act_act_isda(from, to);
    }
    return 0.0;
}

double interest_amount(double balance, double annual_rate, double fraction) {
    return balance * annual_rate * fraction;
}

double annuity_payment(double principal, double annual_rate, int payments) {
    if (annual_rate == 0.0) {
        return principal / payments;
    }
    const double i = annual_rate / 12.0;
    // То же, что 1 - (1 + i)^(-N), но без потери точности при малых ставках.
    const double denominator = -std::expm1(-payments * std::log1p(i));
    return principal * i / denominator;
}

double effective_rate(double rate, std::optional<double> floor, std::optional<double> cap) {
    double result = rate;
    if (floor && result < *floor) {
        result = *floor;
    }
    if (cap && result > *cap) {
        result = *cap;
    }
    return result;
}

std::optional<double> last_known_rate(std::span<const RateFixing> fixings, Date date) {
    const auto after = std::upper_bound(
        fixings.begin(), fixings.end(), date,
        [](Date value, const RateFixing& fixing) { return value < fixing.date; });
    if (after == fixings.begin()) {
        return std::nullopt;
    }
    return std::prev(after)->value;
}

double apply_principal(double& balance, double amount) {
    const double paid = std::clamp(amount, 0.0, balance);
    balance -= paid;
    return paid;
}

double round_money(double amount) {
    return std::round(amount * 100.0) / 100.0;
}

}  
