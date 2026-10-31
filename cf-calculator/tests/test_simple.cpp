#include "test_helpers.hpp"

namespace cf::testing {

TEST(SimpleCalculatorTest, Deal15_PassiveBulletDeposit) {
    DealInput deal{};
    deal.deal_id = 15;
    deal.report_date = D("2022-08-31");
    deal.deal_date = D("2022-03-14");
    deal.maturity_date = D("2023-03-14");
    deal.discriminator = DealType::SIMPLE;
    deal.rate_type = RateType::FIXED;
    deal.day_convention = DayCount::ACT_ACT_ISDA;
    deal.current_principal = -1896648.88;
    deal.interest_rate = 0.04;

    deal.interest_rule.start_date = D("2022-03-14");
    deal.interest_rule.period_months = 1;
    deal.interest_rule.day_num = 14;
    deal.regular_rule.is_end_date = true;

    const auto rows = calculate_cash_flows(deal);
    ASSERT_EQ(rows.size(), 7u);
    verify_schedule_invariants(deal, rows, /*expect_full_principal_repayment=*/true);

    expect_row_matches_etalon(rows[0], "2022-09-14", true, false,
        -1896648.88, 14.0 / 365.0, 0.04, -2909.927049, 0.0, -1896648.88, -2909.927049);
    expect_row_matches_etalon(rows[1], "2022-10-14", true, false,
        -1896648.88, 30.0 / 365.0, 0.04, -6235.557962, 0.0, -1896648.88, -6235.557962);
    expect_row_matches_etalon(rows[2], "2022-11-14", true, false,
        -1896648.88, 31.0 / 365.0, 0.04, -6443.409894, 0.0, -1896648.88, -6443.409894);

    expect_row_matches_etalon(rows.back(), "2023-03-14", true, true,
        -1896648.88, 28.0 / 365.0, 0.04, -5819.854098, -1896648.88, 0.0, -1902468.734098);
}

TEST(SimpleCalculatorTest, Deal16_ExplicitPrincipalSchedule) {
    DealInput deal{};
    deal.deal_id = 16;
    deal.report_date = D("2022-08-31");
    deal.deal_date = D("2020-05-07");
    deal.maturity_date = D("2023-04-30");
    deal.discriminator = DealType::SIMPLE;
    deal.rate_type = RateType::FIXED;
    deal.day_convention = DayCount::ACT_ACT_ISDA;
    deal.current_principal = 1047077.30;
    deal.interest_rate = 0.061;

    deal.interest_rule.start_date = D("2020-05-07");
    deal.interest_rule.period_months = 1;
    deal.interest_rule.day_num = 7;

    deal.explicit_principal_steps = {
        {D("2022-12-31"), 250000.0},
        {D("2023-01-31"), 250000.0},
        {D("2023-02-28"), 250000.0},
        {D("2023-04-30"), 297077.30}
    };

    const auto rows = calculate_cash_flows(deal);
    ASSERT_EQ(rows.size(), 12u);
    verify_schedule_invariants(deal, rows, /*expect_full_principal_repayment=*/true);

    expect_row_matches_etalon(rows[0], "2022-09-07", true, false,
        1047077.30, 7.0 / 365.0, 0.061, 1224.937006, 0.0, 1047077.30, 1224.937006);
    expect_row_matches_etalon(rows[3], "2022-12-07", true, false,
        1047077.30, 30.0 / 365.0, 0.061, 5249.730025, 0.0, 1047077.30, 5249.730025);

    expect_row_matches_etalon(rows[4], "2022-12-31", false, true,
        1047077.30, 24.0 / 365.0, 0.061, 0.0, 250000.0, 797077.30, 250000.0);
    EXPECT_GT(rows[4].interest_accum_after, 0.0);
}

TEST(SimpleCalculatorTest, Deal20_ExplicitScheduleExceedsPrincipalAnomaly) {
    DealInput deal{};
    deal.deal_id = 20;
    deal.report_date = D("2022-08-31");
    deal.deal_date = D("2020-05-07");
    deal.maturity_date = D("2023-04-30");
    deal.discriminator = DealType::SIMPLE;
    deal.rate_type = RateType::FIXED;
    deal.day_convention = DayCount::ACT_ACT_ISDA;
    deal.current_principal = 1047077.30;
    deal.interest_rate = 0.061;

    deal.interest_rule.start_date = D("2020-05-07");
    deal.interest_rule.period_months = 1;
    deal.interest_rule.day_num = 7;

    deal.explicit_principal_steps = {
        {D("2022-12-31"), 250000.0},
        {D("2023-01-31"), 250000.0},
        {D("2023-02-28"), 250000.0},
        {D("2023-04-30"), 356179.0}
    };

    const auto rows = calculate_cash_flows(deal);
    verify_schedule_invariants(deal, rows, /*expect_full_principal_repayment=*/false);

    EXPECT_NEAR(rows.back().principal_after, -59101.70, kTolAmount);
}

} // namespace cf::testing
