#include <gtest/gtest.h>

#include <vector>

#include "cfcore/date.hpp"
#include "cfcore/finance.hpp"

using namespace cfcore;

TEST(YearFraction, Act365CountsActualDays) {
    EXPECT_DOUBLE_EQ(year_fraction(from_ymd(2022, 8, 22), from_ymd(2022, 9, 22), DayCount::Act365), 31.0 / 365.0);
}

TEST(YearFraction, Act365FullLeapYearExceedsOne) {
    EXPECT_DOUBLE_EQ(year_fraction(from_ymd(2024, 1, 1), from_ymd(2025, 1, 1), DayCount::Act365), 366.0 / 365.0);
}

TEST(YearFraction, IsdaFullLeapYearIsOne) {
    EXPECT_DOUBLE_EQ(year_fraction(from_ymd(2024, 1, 1), from_ymd(2025, 1, 1), DayCount::ActActIsda), 1.0);
}

TEST(YearFraction, IsdaSplitsPeriodByCalendarYears) {
    const double expected = 31.0 / 365.0 + 31.0 / 366.0;
    EXPECT_DOUBLE_EQ(year_fraction(from_ymd(2023, 12, 1), from_ymd(2024, 2, 1), DayCount::ActActIsda), expected);
}

TEST(YearFraction, IsdaIsAdditiveAcrossSplitPoint) {
    const Date a = from_ymd(2023, 3, 15), b = from_ymd(2024, 2, 29), c = from_ymd(2025, 6, 1);
    const double whole = year_fraction(a, c, DayCount::ActActIsda);
    const double parts = year_fraction(a, b, DayCount::ActActIsda) + year_fraction(b, c, DayCount::ActActIsda);
    EXPECT_NEAR(whole, parts, 1e-14);
}

TEST(YearFraction, NonPositivePeriodIsZero) {
    const Date d = from_ymd(2022, 8, 31);
    EXPECT_EQ(year_fraction(d, d, DayCount::Act365), 0.0);
    EXPECT_EQ(year_fraction(d + 1, d, DayCount::ActActIsda), 0.0);
}

TEST(Interest, FirstCouponOfDealOne) {
    const double fraction = year_fraction(from_ymd(2022, 8, 22), from_ymd(2022, 9, 22), DayCount::Act365);
    EXPECT_NEAR(interest_amount(3646037.89, 0.075, fraction), 23224.761902054797, 1e-9);
}

TEST(Annuity, DealOnePayment) {
    EXPECT_NEAR(annuity_payment(3646037.89, 0.075, 334), 26037.32199991525, 1e-9);
}

TEST(Annuity, ZeroRateIsEqualParts) {
    EXPECT_DOUBLE_EQ(annuity_payment(1200.0, 0.0, 12), 100.0);
}

TEST(Annuity, TinyRateStaysAccurate) {
    const double principal = 3646037.89, rate = 1e-12;
    const int n = 360;
    const double i = rate / 12.0;
    const double expected = principal / n * (1.0 + (n + 1) / 2.0 * i);
    EXPECT_NEAR(annuity_payment(principal, rate, n), expected, 1e-9);
}

TEST(Annuity, PaymentsDiscountBackToPrincipal) {
    const double principal = 1000000.0, rate = 0.12;
    const int n = 24;
    const double pmt = annuity_payment(principal, rate, n);
    double balance = principal;
    for (int j = 0; j < n; ++j) {
        balance = balance * (1.0 + rate / 12.0) - pmt;
    }
    EXPECT_NEAR(balance, 0.0, 1e-6);
}

TEST(EffectiveRate, AppliesFloorAndCap) {
    EXPECT_DOUBLE_EQ(effective_rate(0.05, 0.06, 0.10), 0.06);
    EXPECT_DOUBLE_EQ(effective_rate(0.15, 0.06, 0.10), 0.10);
    EXPECT_DOUBLE_EQ(effective_rate(0.08, 0.06, 0.10), 0.08);
}

TEST(EffectiveRate, MissingLimitsAreIgnored) {
    EXPECT_DOUBLE_EQ(effective_rate(0.05, std::nullopt, std::nullopt), 0.05);
    EXPECT_DOUBLE_EQ(effective_rate(0.05, std::nullopt, 0.04), 0.04);
    EXPECT_DOUBLE_EQ(effective_rate(0.05, 0.06, std::nullopt), 0.06);
}

TEST(LastKnownRate, TakesLatestNotAfterDate) {
    const std::vector<RateFixing> fixings = {{100, 0.08}, {110, 0.075}, {120, 0.07}};
    EXPECT_EQ(last_known_rate(fixings, 110), 0.075);
    EXPECT_EQ(last_known_rate(fixings, 115), 0.075);
    EXPECT_EQ(last_known_rate(fixings, 500), 0.07);
}

TEST(LastKnownRate, EmptyBeforeFirstFixing) {
    const std::vector<RateFixing> fixings = {{100, 0.08}};
    EXPECT_FALSE(last_known_rate(fixings, 99).has_value());
    EXPECT_FALSE(last_known_rate({}, 99).has_value());
}

TEST(ApplyPrincipal, ReducesBalance) {
    double balance = 100.0;
    EXPECT_DOUBLE_EQ(apply_principal(balance, 30.0), 30.0);
    EXPECT_DOUBLE_EQ(balance, 70.0);
}

TEST(ApplyPrincipal, NeverGoesBelowZero) {
    double balance = 100.0;
    EXPECT_DOUBLE_EQ(apply_principal(balance, 150.0), 100.0);
    EXPECT_EQ(balance, 0.0);
}

TEST(ApplyPrincipal, NegativeAmountRepaysNothing) {
    double balance = 100.0;
    EXPECT_EQ(apply_principal(balance, -25003.0), 0.0);
    EXPECT_DOUBLE_EQ(balance, 100.0);
}

TEST(RoundMoney, RoundsHalfAwayFromZero) {
    EXPECT_DOUBLE_EQ(round_money(0.125), 0.13);
    EXPECT_DOUBLE_EQ(round_money(-0.125), -0.13);
    EXPECT_DOUBLE_EQ(round_money(26037.32199991525), 26037.32);
}

TEST(NeumaierSum, KeepsSmallTermsNextToHugeOne) {
    NeumaierSum sum;
    sum.add(1e16);
    for (int i = 0; i < 1000000; ++i) {
        sum.add(1.0);
    }
    sum.add(-1e16);
    EXPECT_EQ(sum.value(), 1000000.0);
}

TEST(NeumaierSum, HandlesTermLargerThanSum) {
    NeumaierSum sum;
    sum.add(1.0);
    sum.add(1e100);
    sum.add(1.0);
    sum.add(-1e100);
    EXPECT_EQ(sum.value(), 2.0);
}

TEST(NeumaierSum, MergeKeepsCompensation) {
    NeumaierSum left, right;
    left.add(1e16);
    for (int i = 0; i < 1000; ++i) {
        right.add(1.0);
    }
    left.merge(right);
    left.add(-1e16);
    EXPECT_EQ(left.value(), 1000.0);
}

TEST(NeumaierSum, EmptyIsZero) {
    EXPECT_EQ(NeumaierSum{}.value(), 0.0);
}
