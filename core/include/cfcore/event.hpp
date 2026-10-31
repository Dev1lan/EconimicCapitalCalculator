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
