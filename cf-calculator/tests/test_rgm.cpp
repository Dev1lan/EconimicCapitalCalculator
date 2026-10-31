#include "test_helpers.hpp"

namespace cf::testing {

TEST(RgmCalculatorTest, Deal2_SynchronousEqualPrincipal) {
    DealInput deal{};
    deal.deal_id = 2;
    deal.report_date = D("2022-08-31");
    deal.deal_date = D("2020-06-22");
    deal.maturity_date = D("2050-06-15");
    deal.discriminator = DealType::RGM;
    deal.rate_type = RateType::FIXED;
    deal.day_convention = DayCount::ACT_ACT_ISDA;
    deal.current_principal = 3646037.89;
    deal.interest_rate = 0.075;

    deal.regular_rule.start_date = D("2020-06-22");
    deal.regular_rule.period_months = 1;

    const auto rows = calculate_cash_flows(deal);
    ASSERT_EQ(rows.size(), 334u);
    verify_schedule_invariants(deal, rows, /*expect_full_principal_repayment=*/true);

    expect_row_matches_etalon(rows[0], "2022-09-22", true, true,
        3646037.89, 0.060274, 0.075, 16482.089092, 10916.281108, 3635121.608892, 27398.370200);
    expect_row_matches_etalon(rows[1], "2022-10-22", true, true,
        3635121.608892, 0.082192, 0.075, 22408.283890, 10916.281108, 3624205.327784, 33324.564998);
    expect_row_matches_etalon(rows[2], "2022-11-22", true, true,
        3624205.327784, 0.084932, 0.075, 23085.691472, 10916.281108, 3613289.046677, 34001.972579);
    expect_row_matches_etalon(rows[3], "2022-12-22", true, true,
        3613289.046677, 0.082192, 0.075, 22273.699603, 10916.281108, 3602372.765569, 33189.980711);
    expect_row_matches_etalon(rows[4], "2023-01-22", true, true,
        3602372.765569, 0.084932, 0.075, 22946.621041, 10916.281108, 3591456.484461, 33862.902149);
}

TEST(RgmCalculatorTest, Deal22_AsynchronousInterestAndPrincipalDates) {
    DealInput deal{};
    deal.deal_id = 22;
    deal.report_date = D("2022-08-31");
    deal.deal_date = D("2021-04-30");
    deal.maturity_date = D("2023-04-30");
    deal.discriminator = DealType::RGM;
    deal.rate_type = RateType::FLOAT;
    deal.day_convention = DayCount::ACT_ACT_ISDA;
    deal.current_principal = 1000000.0;
    deal.interest_rate = 0.11;
    deal.cap_rate = 1.0;
    deal.floor_rate = 0.0;

    deal.interest_rule.start_date = D("2021-05-10");
    deal.interest_rule.period_months = 1;
    deal.interest_rule.day_num = 10;

    deal.regular_rule.start_date = D("2021-05-20");
    deal.regular_rule.period_months = 1;
    deal.regular_rule.day_num = 20;

    const auto rows = calculate_cash_flows(deal);
    ASSERT_EQ(rows.size(), 17u);
    verify_schedule_invariants(deal, rows, /*expect_full_principal_repayment=*/true);

    expect_row_matches_etalon(rows[0], "2022-09-10", true, false,
        1000000.0, 10.0 / 365.0, 0.19, 5205.479452, 0.0, 1000000.0, 5205.479452);

    expect_row_matches_etalon(rows[1], "2022-09-20", false, true,
        1000000.0, 10.0 / 365.0, 0.19, 0.0, 111111.111111, 888888.888889, 111111.111111);
    EXPECT_NEAR(rows[1].interest_accum_after, 5205.479452, kTolAmount);

    expect_row_matches_etalon(rows[2], "2022-10-10", true, false,
        888888.888889, 20.0 / 365.0, 0.19, 14459.665145, 0.0, 888888.888889, 14459.665145);

    expect_row_matches_etalon(rows[3], "2022-10-20", false, true,
        888888.888889, 10.0 / 365.0, 0.19, 0.0, 111111.111111, 777777.777778, 111111.111111);

    expect_row_matches_etalon(rows[4], "2022-11-10", true, false,
        777777.777778, 21.0 / 365.0, 0.19, 13129.375951, 0.0, 777777.777778, 13129.375951);
}

} // namespace cf::testing
