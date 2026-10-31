#pragma once

#include <vector>

#include "cfcore/engine.hpp"

namespace cfcore::internal {

inline constexpr int kMaxEvents = 3600;

// Что нужно расчётному циклу кроме списка событий.
struct ScheduleInfo {
    Date accrual_start = 0;       // откуда начисляются проценты первого периода
    Date last_interest_date = 0;  // последняя процентная дата не позже отчёта
    bool has_past_interest = false;  // такая дата есть в графике
    int principal_events = 0;
    double rate_component = 0.0;  // ставка сделки на начало начисления
};

// Строит будущие события сделки, отсортированные по дате и типу.
ErrorCode build_events(const Deal& deal, const DealArena& arena, const Policy& policy,
                       std::vector<Event>& events, ScheduleInfo& info);

}  // namespace cfcore::internal
