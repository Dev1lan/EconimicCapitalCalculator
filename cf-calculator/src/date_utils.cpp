#include "date_utils.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <stdexcept>

namespace cf::dates {

int32_t to_date32(int year, unsigned month, unsigned day) noexcept {
    using namespace std::chrono;
    const year_month_day ymd{std::chrono::year{year}, std::chrono::month{month}, std::chrono::day{day}};
    return static_cast<int32_t>(sys_days{ymd}.time_since_epoch().count());
}

YMD from_date32(int32_t days_since_epoch) noexcept {
    using namespace std::chrono;
    const sys_days sd{days{days_since_epoch}};
    const year_month_day ymd{sd};
    return YMD{
        static_cast<int>(ymd.year()),
        static_cast<unsigned>(ymd.month()),
        static_cast<unsigned>(ymd.day())
    };
}

std::string format_iso(int32_t date) {
    const YMD ymd = from_date32(date);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", ymd.year, ymd.month, ymd.day);
    return std::string(buf);
}

int32_t parse_iso(const std::string& s) {
    int y = 0, m = 0, d = 0;
    if (s.size() >= 10 && s[4] == '-' && s[7] == '-') {
        // YYYY-MM-DD
        if (std::sscanf(s.c_str(), "%d-%d-%d", &y, &m, &d) == 3) {
            return to_date32(y, static_cast<unsigned>(m), static_cast<unsigned>(d));
        }
    } else if (s.size() >= 10 && s[2] == '.' && s[5] == '.') {
        // DD.MM.YYYY
        if (std::sscanf(s.c_str(), "%d.%d.%d", &d, &m, &y) == 3) {
            return to_date32(y, static_cast<unsigned>(m), static_cast<unsigned>(d));
        }
    }
    throw std::invalid_argument("Invalid date string format: " + s);
}

bool is_leap_year(int year) noexcept {
    return std::chrono::year{year}.is_leap();
}

unsigned last_day_of_month(int year, unsigned month) noexcept {
    using namespace std::chrono;
    const year_month_day_last ymdl{std::chrono::year{year}, month_day_last{std::chrono::month{month}}};
    return static_cast<unsigned>(ymdl.day());
}

int32_t add_months_pinned(int32_t start_date, int32_t months_to_add, std::optional<int32_t> day_num) noexcept {
    const YMD base = from_date32(start_date);

    const int64_t total_months = static_cast<int64_t>(base.year) * 12 +
                                 static_cast<int64_t>(base.month - 1) +
                                 static_cast<int64_t>(months_to_add);

    int target_year = static_cast<int>(total_months / 12);
    int rem_month = static_cast<int>(total_months % 12);
    if (rem_month < 0) {
        rem_month += 12;
        --target_year;
    }
    const unsigned target_month = static_cast<unsigned>(rem_month + 1);
    const unsigned max_day = last_day_of_month(target_year, target_month);

    unsigned target_day = base.day;
    if (day_num.has_value()) {
        if (*day_num < 0) {
            // day_num == -1 - последний день месяца
            target_day = max_day;
        } else if (*day_num > 0) {
            target_day = static_cast<unsigned>(*day_num);
        }
    }

    if (target_day > max_day) {
        target_day = max_day;
    }

    return to_date32(target_year, target_month, target_day);
}

double year_fraction(int32_t start_date, int32_t end_date, DayCount convention) noexcept {
    if (end_date <= start_date) {
        return 0.0;
    }

    if (convention == DayCount::ACT_365) {
        return static_cast<double>(end_date - start_date) / 365.0;
    }

    const YMD start_ymd = from_date32(start_date);
    const YMD end_ymd = from_date32(end_date);

    if (start_ymd.year == end_ymd.year) {
        const double denom = is_leap_year(start_ymd.year) ? 366.0 : 365.0;
        return static_cast<double>(end_date - start_date) / denom;
    }

    double total_yf = 0.0;
    int32_t cur = start_date;

    for (int y = start_ymd.year; y <= end_ymd.year; ++y) {
        const int32_t next_year_start = to_date32(y + 1, 1, 1);
        const int32_t seg_end = std::min(end_date, next_year_start);
        if (seg_end > cur) {
            const double denom = is_leap_year(y) ? 366.0 : 365.0;
            total_yf += static_cast<double>(seg_end - cur) / denom;
            cur = seg_end;
        }
        if (cur >= end_date) {
            break;
        }
    }

    return total_yf;
}

std::vector<int32_t> generate_rule_dates(
    const ScheduleRule& rule,
    int32_t report_date,
    int32_t maturity_date,
    bool include_maturity
) {
    std::vector<int32_t> result;
    if (maturity_date <= report_date) {
        return result;
    }

    // Если задан код ENDDATE — единственный платеж в дату погашения
    if (rule.is_end_date) {
        result.push_back(maturity_date);
        return result;
    }

    // 1. Генерация с шагом в днях
    if (rule.start_date.has_value() && rule.period_days.value_or(0) > 0) {
        const int32_t step_days = *rule.period_days;
        int32_t current = *rule.start_date;

        if (current <= report_date) {
            const int32_t diff = report_date - current;
            const int32_t steps = diff / step_days + 1;
            current += steps * step_days;
        }

        while (current <= maturity_date) {
            if (current > report_date) {
                result.push_back(current);
            }
            current += step_days;
        }
    }
    // 2. Генерация с шагом в месяцах
    else if (rule.start_date.has_value() && rule.period_months.value_or(0) > 0) {
        const int32_t step_months = *rule.period_months;
        const int32_t anchor = *rule.start_date;

        for (int32_t k = 0; ; ++k) {
            const int32_t candidate = add_months_pinned(anchor, k * step_months, rule.day_num);
            if (candidate > maturity_date) {
                break;
            }
            if (candidate > report_date) {
                result.push_back(candidate);
            }
        }
    }

    if (include_maturity) {
        if (result.empty() || result.back() != maturity_date) {
            result.push_back(maturity_date);
        }
    }

    // Сортировка и удаление дубликатов
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());

    return result;
}

std::vector<ScheduleEvent> build_merged_timeline(
    const std::vector<int32_t>& interest_dates,
    const std::vector<int32_t>& principal_dates,
    const std::vector<PrincipalStep>& explicit_principal_steps,
    const std::vector<int32_t>& repricing_dates,
    int32_t report_date,
    int32_t maturity_date
) {
    std::map<int32_t, ScheduleEvent> timeline;

    auto get_or_create = [&](int32_t d) -> ScheduleEvent& {
        auto& ev = timeline[d];
        ev.date = d;
        return ev;
    };

    for (int32_t d : interest_dates) {
        if (d > report_date && d <= maturity_date) {
            get_or_create(d).is_interest = true;
        }
    }

    for (int32_t d : principal_dates) {
        if (d > report_date && d <= maturity_date) {
            get_or_create(d).is_principal = true;
        }
    }

    for (const auto& step : explicit_principal_steps) {
        if (step.date > report_date && step.date <= maturity_date) {
            auto& ev = get_or_create(step.date);
            ev.is_principal = true;
            ev.explicit_principal = ev.explicit_principal.value_or(0.0) + step.amount;
        }
    }

    for (int32_t d : repricing_dates) {
        if (d > report_date && d <= maturity_date) {
            get_or_create(d).is_repricing = true;
        }
    }

    std::vector<ScheduleEvent> merged;
    merged.reserve(timeline.size());
    for (const auto& [date, ev] : timeline) {
        merged.push_back(ev);
    }

    return merged;
}

} // namespace cf::dates
