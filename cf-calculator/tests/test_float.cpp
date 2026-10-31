#include "test_helpers.hpp"

namespace cf::testing {

TEST(FloatCalculatorTest, Deal25_CapLimitAppliedCorrectly) {
    DealInput deal{};
    deal.deal_id = 25;
    deal.report_date = D("2022-08-31");
    deal.deal_date = D("2022-01-01");
    deal.maturity_date = D("2023-01-01");
    deal.discriminator = DealType::SIMPLE;
    deal.rate_type = RateType::FLOAT;
    deal.day_convention = DayCount::ACT_365;
    deal.current_principal = 500000.0;
    deal.interest_rate = 0.05;
    deal.base_rate = 0.08;
    deal.cap_rate = 0.10;
    deal.floor_rate = 0.06;

    deal.interest_rule.start_date = D("2022-01-01");
    deal.interest_rule.period_months = 1;
    deal.interest_rule.day_num = 1;
    deal.regular_rule.is_end_date = true;

    const auto rows = calculate_cash_flows(deal);
    ASSERT_EQ(rows.size(), 5u);
    verify_schedule_invariants(deal, rows, /*expect_full_principal_repayment=*/true);

    expect_row_matches_etalon(rows[0], "2022-09-01", true, false,
        500000.0, 1.0 / 365.0, 0.10, 136.986301, 0.0, 500000.0, 136.986301);
    expect_row_matches_etalon(rows[1], "2022-10-01", true, false,
        500000.0, 30.0 / 365.0, 0.10, 4109.589041, 0.0, 500000.0, 4109.589041);
    expect_row_matches_etalon(rows[2], "2022-11-01", true, false,
        500000.0, 31.0 / 365.0, 0.10, 4246.575342, 0.0, 500000.0, 4246.575342);
    expect_row_matches_etalon(rows[3], "2022-12-01", true, false,
        500000.0, 30.0 / 365.0, 0.10, 4109.589041, 0.0, 500000.0, 4109.589041);
    expect_row_matches_etalon(rows[4], "2023-01-01", true, true,
        500000.0, 31.0 / 365.0, 0.10, 4246.575342, 500000.0, 0.0, 504246.575342);
}

TEST(FloatCalculatorTest, RepricingEventDoesNotCreateCashFlowAndSwitchesRate) {
    DealInput deal{};
    deal.deal_id = 30;
    deal.report_date = D("2022-08-31");
    deal.deal_date = D("2022-01-01");
    deal.maturity_date = D("2023-01-01");
    deal.discriminator = DealType::SIMPLE;
    deal.rate_type = RateType::FLOAT;
    deal.day_convention = DayCount::ACT_365;
    deal.current_principal = 500000.0;
    deal.interest_rate = 0.05;
    deal.base_rate = 0.08;

    deal.interest_rule.is_end_date = true;
    deal.regular_rule.is_end_date = true;

    deal.explicit_repricing_rates = {
        {D("2022-08-31"), 0.13},
        {D("2022-10-15"), 0.15}
    };

    const auto rows = calculate_cash_flows(deal);
    ASSERT_EQ(rows.size(), 2u);
    verify_schedule_invariants(deal, rows, /*expect_full_principal_repayment=*/true);

    EXPECT_EQ(dates::format_iso(rows[0].cf_date), "2022-10-15");
    EXPECT_FALSE(rows[0].is_interest);
    EXPECT_FALSE(rows[0].is_principal);
    EXPECT_NEAR(rows[0].cf_rate, 0.13, kTolRate);
    EXPECT_NEAR(rows[0].interest_accrued, 500000.0 * 0.13 * (45.0 / 365.0), kTolAmount);
    EXPECT_NEAR(rows[0].interest_paid, 0.0, kTolAmount);
    EXPECT_NEAR(rows[0].principal_paid, 0.0, kTolAmount);
    EXPECT_NEAR(rows[0].total_cf, 0.0, kTolAmount);

    EXPECT_EQ(dates::format_iso(rows[1].cf_date), "2023-01-01");
    EXPECT_TRUE(rows[1].is_interest);
    EXPECT_TRUE(rows[1].is_principal);
    EXPECT_NEAR(rows[1].cf_rate, 0.15, kTolRate);
    const double expected_total_interest =
        500000.0 * 0.13 * (45.0 / 365.0) + 500000.0 * 0.15 * (78.0 / 365.0);
    EXPECT_NEAR(rows[1].interest_paid, expected_total_interest, kTolAmount);
    EXPECT_NEAR(rows[1].principal_paid, 500000.0, kTolAmount);
}

TEST(FloatCalculatorTest, Deal23_ZeroBaseRateFallbackToDefaultKeyRate) {
    DealInput deal{};
    deal.deal_id = 23;
    deal.report_date = D("2022-08-31");
    deal.deal_date = D("2021-05-31");
    deal.maturity_date = D("2023-05-31");
    deal.discriminator = DealType::RGM;
    deal.rate_type = RateType::FLOAT;
    deal.day_convention = DayCount::ACT_ACT_ISDA;
    deal.current_principal = 1000000.0;
    deal.interest_rate = 0.11;
    deal.base_rate = 0.0;

    deal.regular_rule.start_date = D("2021-05-31");
    deal.regular_rule.period_months = 1;
    deal.regular_rule.day_num = 31;

    const auto rows = calculate_cash_flows(deal);
    ASSERT_EQ(rows.size(), 9u);
    verify_schedule_invariants(deal, rows, /*expect_full_principal_repayment=*/true);

    for (const auto& r : rows) {
        EXPECT_NEAR(r.cf_rate, 0.19, kTolRate);
    }
}

} // namespace cf::testing
