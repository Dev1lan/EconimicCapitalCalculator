#include "test_helpers.hpp"

namespace cf::testing {

TEST(AnnCalculatorTest, Deal1_LongAnnuityMatchesEtalon) {
    DealInput deal{};
    deal.deal_id = 1;
    deal.report_date = D("2022-08-31");
    deal.deal_date = D("2020-06-22");
    deal.maturity_date = D("2050-06-15");
    deal.discriminator = DealType::ANN;
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
        3646037.89, 0.060274, 0.075, 16482.089092, 9555.232908, 3636482.657092, 26037.322);
    expect_row_matches_etalon(rows[1], "2022-10-22", true, true,
        3636482.657092, 0.082192, 0.075, 22416.673914, 3620.648086, 3632862.009006, 26037.322);
    expect_row_matches_etalon(rows[2], "2022-11-22", true, true,
        3632862.009006, 0.084932, 0.075, 23140.833345, 2896.488655, 3629965.520351, 26037.322);
    expect_row_matches_etalon(rows[3], "2022-12-22", true, true,
        3629965.520351, 0.082192, 0.075, 22376.499783, 3660.822217, 3626304.698134, 26037.322);
    expect_row_matches_etalon(rows[4], "2023-01-22", true, true,
        3626304.698134, 0.084932, 0.075, 23099.064173, 2938.257827, 3623366.440307, 26037.322);
    expect_row_matches_etalon(rows[5], "2023-02-22", true, true,
        3623366.440307, 0.084932, 0.075, 23080.347873, 2956.974127, 3620409.466180, 26037.322);
    expect_row_matches_etalon(rows[6], "2023-03-22", true, true,
        3620409.466180, 0.076712, 0.075, 20829.753093, 5207.568907, 3615201.897273, 26037.322);

    for (size_t i = 0; i + 1 < rows.size(); ++i) {
        EXPECT_NEAR(rows[i].total_cf, 26037.322, kTolAmount);
    }

    EXPECT_EQ(dates::format_iso(rows.back().cf_date), "2050-06-15");
    EXPECT_NEAR(rows.back().principal_paid, rows.back().principal_before, 1e-9);
    EXPECT_NEAR(rows.back().principal_after, 0.0, 1e-9);
}

TEST(AnnCalculatorTest, Deal7_ShortAutoLoanAnnuity) {
    DealInput deal{};
    deal.deal_id = 7;
    deal.report_date = D("2022-08-31");
    deal.deal_date = D("2019-06-03");
    deal.maturity_date = D("2022-12-14");
    deal.discriminator = DealType::ANN;
    deal.rate_type = RateType::FIXED;
    deal.day_convention = DayCount::ACT_ACT_ISDA;
    deal.current_principal = 349553.63;
    deal.interest_rate = 0.15;

    deal.regular_rule.start_date = D("2018-12-14");
    deal.regular_rule.period_months = 1;
    deal.regular_rule.day_num = 14;
    deal.interest_rule = deal.regular_rule;

    const auto rows = calculate_cash_flows(deal);
    ASSERT_EQ(rows.size(), 4u); // 14.09.2022, 14.10.2022, 14.11.2022, 14.12.2022
    verify_schedule_invariants(deal, rows, /*expect_full_principal_repayment=*/true);

    for (size_t i = 1; i + 1 < rows.size(); ++i) {
        EXPECT_NEAR(rows[i].total_cf, rows[0].total_cf, kTolAmount);
    }
    EXPECT_NEAR(rows.back().principal_after, 0.0, 1e-9);
}

} // namespace cf::testing
