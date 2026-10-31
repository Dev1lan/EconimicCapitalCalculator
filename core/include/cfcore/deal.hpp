#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace cfcore {

// Дней от 1970-01-01. Тот же формат, что date32 в Arrow.
using Date = int32_t;

enum class Side : uint8_t { Asset, Liability };
enum class Currency : uint8_t { RUB };
enum class AmortizationType : uint8_t { ANN, RGM, SIMPLE };
enum class RateType : uint8_t { Fixed, Float };
enum class DayCount : uint8_t { Act365, ActActIsda };

// Правило регулярного графика: как из начальной даты получить даты платежей.
struct PaymentRule {
    enum class Kind : uint8_t {
        None,     // правила нет
        Months,   // каждые period месяцев
        Days,     // каждые period дней
        EndDate,  // один платёж на дату погашения
    };

    Kind kind = Kind::None;
    int32_t period = 0;
    Date start = 0;
    int32_t day = 0;  // день месяца; 0 = взять день из start; -1 = последний день
};

// Кусок общего массива. Списки разной длины лежат подряд в DealArena,
// поэтому на каждую сделку не нужно отдельное выделение памяти.
struct Span {
    uint32_t offset = 0;
    uint32_t length = 0;
};

struct PrincipalItem {
    Date date = 0;
    double amount = 0.0;
};

struct ResetItem {
    Date date = 0;
    std::optional<double> fixed_rate;  // ставка из договора, если она уже известна
};

// Значение индикатора на дату, например ключевой ставки.
struct RateFixing {
    Date date = 0;
    double value = 0.0;
};

// Явные графики всех сделок одной порции и общая история индикатора.
struct DealArena {
    std::vector<Date> interest_dates;
    std::vector<PrincipalItem> principal_items;
    std::vector<ResetItem> reset_items;
    std::vector<RateFixing> indicator_fixings;  // по возрастанию даты
};

// Внутренняя модель сделки. Не зависит от названий колонок CSV и Parquet:
// их сопоставление находится в адаптере.
struct Deal {
    int64_t instrument_id = 0;
    int32_t portfolio_key = 0;
    Side side = Side::Asset;
    Currency currency = Currency::RUB;

    double outstanding_principal = 0.0;  // всегда > 0, знак задаёт side
    Date report_date = 0;
    Date start_date = 0;
    Date maturity_date = 0;

    AmortizationType amortization = AmortizationType::ANN;
    RateType rate_type = RateType::Fixed;
    DayCount day_count = DayCount::Act365;

    double rate = 0.0;  // Fixed: ставка; Float: см. Policy::float_rate_is_spread
    std::optional<double> indicator_rate;  // значение индикатора, например КС
    std::optional<double> cap;
    std::optional<double> floor;
    std::optional<double> accrued_interest;  // пусто и явный 0 различаются

    PaymentRule interest_rule;
    PaymentRule principal_rule;
    PaymentRule reset_rule;

    Span interest_schedule;
    Span principal_schedule;
    Span reset_schedule;
};

}  // namespace cfcore
