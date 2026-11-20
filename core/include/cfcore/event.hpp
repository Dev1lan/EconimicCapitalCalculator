#pragma once

#include <cstdint>
#include <optional>

#include "cfcore/deal.hpp"

namespace cfcore {

// Порядок значений задаёт порядок обработки на одной дате:
// сначала проценты по остатку до погашения, затем погашение, затем новая ставка.
enum class EventType : uint8_t { InterestPayment, PrincipalPayment, RateReset };

struct Event {
    Date date = 0;
    EventType type = EventType::InterestPayment;
    std::optional<double> amount;  // пусто: сумма считается в цикле (например, в ANN)
};

enum class RateSource : uint8_t { None, ContractRate, ContractSchedule, FixingHistory, DealIndicator };

// Состояние расчёта на событии. rate_before действует на интервале [period_start, period_end).
struct EventDetail {
    int64_t instrument_id = 0;
    Date date = 0;
    EventType type = EventType::InterestPayment;
    Date period_start = 0;
    Date period_end = 0;
    double year_fraction = 0.0;
    double balance_before = 0.0;
    double balance_after = 0.0;
    double rate_before = 0.0;
    double rate_after = 0.0;
    RateSource rate_source = RateSource::None;  // источник новой ставки при RateReset
    double interest_accrued = 0.0;
    double interest_paid = 0.0;
    double principal_paid = 0.0;
};

enum class FlowType : uint8_t { Principal, Interest };

// Фактический платёж по договору. Бакет считается отдельно.
struct CashFlow {
    int64_t instrument_id = 0;
    Date date = 0;
    FlowType type = FlowType::Principal;
    double amount = 0.0;         // всегда >= 0, направление задаёт Deal::side
    double rate_used = 0.0;      // для процентов
    double year_fraction = 0.0;  // для процентов
};

}  // namespace cfcore
