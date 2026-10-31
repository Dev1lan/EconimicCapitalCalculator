#include "cfcore/date.hpp"

#include <algorithm>
#include <cstdint>

namespace cfcore {

namespace {

// Деление с округлением вниз: для дат до 1970 года и эры до нашей эры.
int64_t floor_div(int64_t a, int64_t b) {
    const int64_t q = a / b;
    return (a % b != 0 && a < 0) ? q - 1 : q;
}

}  // namespace

// Алгоритмы Хиннанта: год считается от марта, поэтому 29 февраля стоит в конце года.
Date from_ymd(int year, int month, int day) {
    const int64_t y = int64_t{year} - (month <= 2 ? 1 : 0);
    const int64_t era = floor_div(y, 400);
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<Date>(era * 146097 + doe - 719468);
}

YearMonthDay to_ymd(Date date) {
    const int64_t z = int64_t{date} + 719468;
    const int64_t era = floor_div(z, 146097);
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    const int month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    const int year = static_cast<int>(yoe + era * 400 + (month <= 2 ? 1 : 0));
    return {year, month, day};
}

bool is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int days_in_month(int year, int month) {
    static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return (month == 2 && is_leap_year(year)) ? 29 : kDays[month - 1];
}

Date add_months(Date date, int months, int day) {
    const YearMonthDay ymd = to_ymd(date);
    const int64_t total = int64_t{ymd.year} * 12 + (ymd.month - 1) + months;
    const int year = static_cast<int>(floor_div(total, 12));
    const int month = static_cast<int>(total - int64_t{year} * 12) + 1;
    return from_ymd(year, month, std::min(day, days_in_month(year, month)));
}

Date add_months(Date date, int months) {
    return add_months(date, months, to_ymd(date).day);
}

}  
