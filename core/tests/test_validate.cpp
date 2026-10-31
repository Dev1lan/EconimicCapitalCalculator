#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "test_helpers.hpp"

using namespace cfcore;
using namespace cfcore::testing;

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();
}

TEST(Validate, AcceptsNormalDeal) {
    EXPECT_EQ(validate(base_deal(AmortizationType::RGM), {}), ErrorCode::Ok);
}

TEST(Validate, RejectsBadBalance) {
    for (const double value : {0.0, -1.0, kNaN, kInf}) {
        Deal deal = base_deal(AmortizationType::RGM);
        deal.outstanding_principal = value;
        EXPECT_EQ(validate(deal, {}), ErrorCode::InvalidNumber) << value;
    }
}

TEST(Validate, RejectsNonFiniteLimitsAndRate) {
    Deal deal = base_deal(AmortizationType::RGM);
    deal.cap = kNaN;
    EXPECT_EQ(validate(deal, {}), ErrorCode::InvalidNumber);

    deal = base_deal(AmortizationType::RGM);
    deal.rate = kInf;
    EXPECT_EQ(validate(deal, {}), ErrorCode::InvalidNumber);
}

TEST(Validate, RejectsFloorAboveCap) {
    Deal deal = base_deal(AmortizationType::RGM);
    deal.floor = 0.2;
    deal.cap = 0.1;
    EXPECT_EQ(validate(deal, {}), ErrorCode::InvalidNumber);
}

TEST(Validate, RejectsMaturityNotAfterReport) {
    Deal deal = base_deal(AmortizationType::RGM);
    deal.maturity_date = deal.report_date;
    EXPECT_EQ(validate(deal, {}), ErrorCode::InvalidDate);
}

TEST(Validate, RejectsDatesOutsideCorridor) {
    Deal deal = base_deal(AmortizationType::RGM);
    deal.maturity_date = 2147483647;
    EXPECT_EQ(validate(deal, {}), ErrorCode::InvalidDate);
}

TEST(Validate, RejectsBadPaymentDay) {
    Deal deal = base_deal(AmortizationType::RGM);
    deal.principal_rule.day = 32;
    EXPECT_EQ(validate(deal, {}), ErrorCode::InvalidSchedule);
    deal.principal_rule.day = -1;
    EXPECT_EQ(validate(deal, {}), ErrorCode::InvalidSchedule);
}

TEST(Validate, ResetRuleMayUseLastDayOfMonth) {
    Deal deal = base_deal(AmortizationType::RGM);
    deal.reset_rule = monthly(deal.start_date, -1);
    EXPECT_EQ(validate(deal, {}), ErrorCode::Ok);
}

TEST(Validate, RejectsSpanOutsideArena) {
    Deal deal = base_deal(AmortizationType::SIMPLE);
    deal.principal_schedule = {0, 3};
    EXPECT_EQ(validate(deal, {}), ErrorCode::InvalidSchedule);
}

TEST(Validate, RejectsUnsortedAndBadListItems) {
    Deal deal = base_deal(AmortizationType::SIMPLE);
    deal.principal_schedule = {0, 2};
    DealArena arena;
    arena.principal_items = {{from_ymd(2022, 10, 1), 1.0}, {from_ymd(2022, 9, 1), 1.0}};
    EXPECT_EQ(validate(deal, arena), ErrorCode::InvalidSchedule);

    arena.principal_items = {{from_ymd(2022, 9, 1), kNaN}, {from_ymd(2022, 10, 1), 1.0}};
    EXPECT_EQ(validate(deal, arena), ErrorCode::InvalidNumber);
}

TEST(Validate, AnnuityIsOnlyMonthly) {
    Deal deal = base_deal(AmortizationType::ANN);
    deal.principal_rule.period = 3;
    EXPECT_EQ(validate(deal, {}), ErrorCode::Unsupported);
}
