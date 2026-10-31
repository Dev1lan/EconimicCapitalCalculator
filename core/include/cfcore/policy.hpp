#pragma once

namespace cfcore {

// Методические правила, которые ещё могут измениться после ответа куратора.
// Значения по умолчанию описывают базовый режим ядра.
struct Policy {
    // Проценты по дням/365 для всех сделок, игнорируя конвенцию сделки.
    bool force_act365 = false;

    // Гасить весь остаток последним платежом.
    bool close_residual = true;

    // ДОПУЩЕНИЕ: первый период процентов идёт от предыдущей платёжной даты.
    bool first_period_from_prev_pay = true;

    // ДОПУЩЕНИЕ: платёж в дату отчёта уже учтён в остатке.
    bool include_payment_on_report = false;

    // ДОПУЩЕНИЕ: у FLOAT итоговая ставка = индикатор + interest_rate.
    bool float_rate_is_spread = true;

    // Правила листа «1 сделка» для сверки с Excel.
    static constexpr Policy excel_etalon() {
        Policy p;
        p.force_act365 = true;
        p.close_residual = false;
        return p;
    }
};

}  // namespace cfcore
