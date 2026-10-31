#include <gtest/gtest.h>

#include "cfcore/engine.hpp"

using namespace cfcore;

TEST(Policy, DefaultModeClosesDebtAndUsesDealConvention) {
    const Policy p;
    EXPECT_TRUE(p.close_residual);
    EXPECT_FALSE(p.force_act365);
}

TEST(Policy, ExcelEtalonMatchesSheetRules) {
    const Policy p = Policy::excel_etalon();
    EXPECT_FALSE(p.close_residual);
    EXPECT_TRUE(p.force_act365);
    EXPECT_TRUE(p.first_period_from_prev_pay);
}

TEST(Deal, EmptyOptionalDiffersFromExplicitZero) {
    Deal deal;
    EXPECT_FALSE(deal.accrued_interest.has_value());
    deal.accrued_interest = 0.0;
    EXPECT_TRUE(deal.accrued_interest.has_value());
}

TEST(Event, SameDateOrderIsInterestPrincipalReset) {
    EXPECT_LT(EventType::InterestPayment, EventType::PrincipalPayment);
    EXPECT_LT(EventType::PrincipalPayment, EventType::RateReset);
}

TEST(ErrorCode, HasStableExportNames) {
    EXPECT_STREQ(to_string(ErrorCode::ScheduleSumMismatch), "SCHEDULE_SUM_MISMATCH");
    EXPECT_STREQ(to_string(InterestSource::Unavailable), "UNAVAILABLE");
}
