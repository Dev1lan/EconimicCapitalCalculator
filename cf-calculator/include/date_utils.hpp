#pragma once

#include "cf_types.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace cf::dates {

struct YMD {
    int year;
    unsigned month;
    unsigned day;
};

int32_t to_date32(int year, unsigned month, unsigned day) noexcept;
YMD from_date32(int32_t days_since_epoch) noexcept;

std::string format_iso(int32_t date);              // "YYYY-MM-DD"
int32_t parse_iso(const std::string& iso_date);    // "YYYY-MM-DD" / "DD.MM.YYYY"

bool is_leap_year(int year) noexcept;
unsigned last_day_of_month(int year, unsigned month) noexcept;

// Сдвиг даты start_date на months_to_add месяцев вперед с фиксацией целевого дня day_num
int32_t add_months_pinned(int32_t start_date, int32_t months_to_add, std::optional<int32_t> day_num) noexcept;

// Расчет доли года (year_fraction) между start_date и end_date по заданной конвенции
double year_fraction(int32_t start_date, int32_t end_date, DayCount convention) noexcept;

// ScheduleRule на (report_date, maturity_date]
std::vector<int32_t> generate_rule_dates(
    const ScheduleRule& rule,
    int32_t report_date,
    int32_t maturity_date,
    bool include_maturity
);

std::vector<ScheduleEvent> build_merged_timeline(
    const std::vector<int32_t>& interest_dates,
    const std::vector<int32_t>& principal_dates,
    const std::vector<PrincipalStep>& explicit_principal_steps,
    const std::vector<int32_t>& repricing_dates,
    int32_t report_date,
    int32_t maturity_date
);

} // namespace cf::dates
