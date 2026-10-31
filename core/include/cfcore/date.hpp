#pragma once

#include "cfcore/deal.hpp"

namespace cfcore {

struct YearMonthDay {
    int year = 1970;
    int month = 1;  // 1..12
    int day = 1;    // 1..31
};

Date from_ymd(int year, int month, int day);
YearMonthDay to_ymd(Date date);

bool is_leap_year(int year);
int days_in_month(int year, int month);

// Сдвиг на months месяцев. Если в месяце нет нужного дня, берётся его последний день.
// day задаёт день месяца результата. Без day используется день исходной даты.
Date add_months(Date date, int months, int day);
Date add_months(Date date, int months);

}  // namespace cfcore
