#pragma once

#include "cf_calculator.hpp"
#include "date_utils.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <numeric>
#include <string>
#include <vector>

namespace cf::testing {

constexpr double kTolAmount = 1e-3;
constexpr double kTolRate   = 1e-6;
constexpr double kTolDebt   = 0.02;

inline int32_t D(const std::string& iso_or_ru_date) {
    return dates::parse_iso(iso_or_ru_date);
}

// Проверка инвариантов для любого графика CF
inline void verify_schedule_invariants(
    const DealInput& deal,
    const std::vector<CashFlowRow>& rows,
    bool expect_full_principal_repayment = true
) {
    ASSERT_FALSE(rows.empty()) << "Schedule must not be empty for valid deal_id=" << deal.deal_id;

    int32_t expected_start = deal.report_date;
    double expected_debt = deal.current_principal;
    double expected_accum = 0.0;
    double total_principal_repaid = 0.0;

    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        SCOPED_TRACE("Row index i=" + std::to_string(i) + ", date=" + dates::format_iso(r.cf_date));

        // 1. Непрерывность периодов и возрастание дат
        EXPECT_EQ(r.deal_id, deal.deal_id);
        EXPECT_EQ(r.period_start, expected_start);
        EXPECT_GT(r.cf_date, r.period_start);

        // 2. Непрерывность остатка основного долга и накопленных процентов
        EXPECT_NEAR(r.principal_before, expected_debt, kTolAmount);
        EXPECT_NEAR(r.interest_accum_before, expected_accum, kTolAmount);

        // 3. Проверка формулы начисления процентов за подпериод
        const double calc_accrued = r.principal_before * r.cf_rate * r.year_fraction;
        EXPECT_NEAR(r.interest_accrued, calc_accrued, kTolAmount);

        // 4. Проверка логики выплаты/накопления процентов
        if (r.is_interest) {
            EXPECT_NEAR(r.interest_paid, r.interest_accum_before + r.interest_accrued, kTolAmount);
            EXPECT_NEAR(r.interest_accum_after, 0.0, kTolAmount);
        } else {
            EXPECT_NEAR(r.interest_paid, 0.0, kTolAmount);
            EXPECT_NEAR(r.interest_accum_after, r.interest_accum_before + r.interest_accrued, kTolAmount);
        }

        // 5. Проверка отсутствия выплаты тела при is_principal == false
        if (!r.is_principal) {
            EXPECT_NEAR(r.principal_paid, 0.0, kTolAmount);
        }

        // 6. Баланс остатка долга и итогового денежного потока
        EXPECT_NEAR(r.principal_after, r.principal_before - r.principal_paid, kTolAmount);
        EXPECT_NEAR(r.total_cf, r.interest_paid + r.principal_paid, kTolAmount);

        total_principal_repaid += r.principal_paid;
        expected_start = r.cf_date;
        expected_debt = r.principal_after;
        expected_accum = r.interest_accum_after;
    }

    // 7. Проверка сохранения основного долга
    if (expect_full_principal_repayment) {
        EXPECT_NEAR(total_principal_repaid, deal.current_principal, kTolDebt);
        EXPECT_NEAR(rows.back().principal_after, 0.0, kTolDebt);
    }
}

inline void expect_row_matches_etalon(
    const CashFlowRow& row,
    const std::string& expected_date,
    bool exp_is_interest,
    bool exp_is_principal,
    double exp_principal_before,
    double exp_year_fraction,
    double exp_cf_rate,
    double exp_interest_paid,
    double exp_principal_paid,
    double exp_principal_after,
    double exp_total_cf
) {
    EXPECT_EQ(dates::format_iso(row.cf_date), expected_date);
    EXPECT_EQ(row.is_interest, exp_is_interest);
    EXPECT_EQ(row.is_principal, exp_is_principal);
    EXPECT_NEAR(row.principal_before, exp_principal_before, kTolAmount);
    EXPECT_NEAR(row.year_fraction, exp_year_fraction, kTolRate);
    EXPECT_NEAR(row.cf_rate, exp_cf_rate, kTolRate);
    EXPECT_NEAR(row.interest_paid, exp_interest_paid, kTolAmount);
    EXPECT_NEAR(row.principal_paid, exp_principal_paid, kTolAmount);
    EXPECT_NEAR(row.principal_after, exp_principal_after, kTolAmount);
    EXPECT_NEAR(row.total_cf, exp_total_cf, kTolAmount);
}

} // namespace cf::testing
