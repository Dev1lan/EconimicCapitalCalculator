#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "test_helpers.hpp"

using namespace cfcore;
using namespace cfcore::testing;

namespace {

std::vector<CashFlow> flows_of(const Deal& deal, const DealArena& arena = {}, const Policy& policy = {}) {
    std::vector<CashFlow> flows;
    EXPECT_EQ(generate_cashflows(deal, arena, policy, flows), ErrorCode::Ok);
    return flows;
}

}  // namespace

// Ручной пример: разовое погашение, проценты каждый месяц на постоянный остаток.
TEST(Settlement, BulletLoanByHand) {
    Deal deal = base_deal(AmortizationType::SIMPLE);
    deal.principal_rule = end_date();
    deal.day_count = DayCount::Act365;

    const auto flows = flows_of(deal);
    const auto interest = only(flows, FlowType::Interest);
    const auto principal = only(flows, FlowType::Principal);

    ASSERT_EQ(principal.size(), 1u);
    EXPECT_EQ(principal[0].date, deal.maturity_date);
    EXPECT_DOUBLE_EQ(principal[0].amount, 1200000.0);

    // Предыдущая дата 31.08, платежи 30.09, 31.10, 30.11 и 31.12.
    ASSERT_EQ(interest.size(), 4u);
    EXPECT_EQ(interest[0].date, from_ymd(2022, 9, 30));
    EXPECT_NEAR(interest[0].amount, 1200000.0 * 0.12 * 30.0 / 365.0, 1e-9);
    EXPECT_NEAR(interest[3].amount, 1200000.0 * 0.12 * 31.0 / 365.0, 1e-9);
}

TEST(Settlement, InterestUsesBalanceBeforeSameDayRepayment) {
    Deal deal = base_deal(AmortizationType::RGM);
    deal.day_count = DayCount::Act365;
    const auto flows = flows_of(deal);
    ASSERT_GE(flows.size(), 4u);
    // На одной дате сначала проценты, затем долг.
    EXPECT_EQ(flows[0].type, FlowType::Interest);
    EXPECT_EQ(flows[1].type, FlowType::Principal);
    EXPECT_NEAR(flows[0].amount, 1200000.0 * 0.12 * 30.0 / 365.0, 1e-9);
    EXPECT_NEAR(flows[2].amount, 900000.0 * 0.12 * 31.0 / 365.0, 1e-9);
}

TEST(Settlement, RepaymentBetweenInterestDatesSplitsAccrual) {
    Deal deal = base_deal(AmortizationType::SIMPLE);
    deal.day_count = DayCount::Act365;
    deal.maturity_date = from_ymd(2022, 9, 30);
    deal.start_date = from_ymd(2022, 8, 31);
    deal.interest_rule = end_date();
    deal.principal_rule = PaymentRule{};
    deal.principal_schedule = {0, 2};
    DealArena arena;
    arena.principal_items = {{from_ymd(2022, 9, 15), 400000.0}, {from_ymd(2022, 9, 30), 800000.0}};

    const auto interest = only(flows_of(deal, arena), FlowType::Interest);
    ASSERT_EQ(interest.size(), 1u);
    const double expected = 1200000.0 * 0.12 * 15.0 / 365.0 + 800000.0 * 0.12 * 15.0 / 365.0;
    EXPECT_NEAR(interest[0].amount, expected, 1e-9);
}

TEST(Settlement, PaymentOnReportDateIsSkippedByDefault) {
    Deal deal = base_deal(AmortizationType::SIMPLE);
    deal.principal_rule = end_date();
    deal.interest_rule = monthly(from_ymd(2022, 1, 31));
    const auto flows = flows_of(deal);
    EXPECT_GT(flows.front().date, deal.report_date);

    Policy policy;
    policy.include_payment_on_report = true;
    const auto with_report = flows_of(deal, {}, policy);
    EXPECT_EQ(with_report.front().date, deal.report_date);
}

TEST(Settlement, FailedDealLeavesOutputUntouched) {
    Deal deal = base_deal(AmortizationType::RGM);
    deal.outstanding_principal = -5.0;
    std::vector<CashFlow> flows(3);
    EXPECT_EQ(generate_cashflows(deal, {}, Policy{}, flows), ErrorCode::InvalidNumber);
    EXPECT_EQ(flows.size(), 3u);
}

TEST(Rgm, EqualPartsAndFullRepayment) {
    const Deal deal = base_deal(AmortizationType::RGM);
    const auto principal = only(flows_of(deal), FlowType::Principal);
    ASSERT_EQ(principal.size(), 4u);
    EXPECT_DOUBLE_EQ(principal[0].amount, 300000.0);
    EXPECT_DOUBLE_EQ(principal[2].amount, 300000.0);
    EXPECT_NEAR(total(principal, FlowType::Principal), 1200000.0, 1e-6);
}

TEST(Rgm, FollowsInterestRuleWhenRegularRuleIsEmpty) {
    Deal deal = base_deal(AmortizationType::RGM);
    deal.principal_rule = PaymentRule{};
    EXPECT_EQ(only(flows_of(deal), FlowType::Principal).size(), 4u);
}

TEST(Annuity, ConstantPaymentExceptLast) {
    const Deal deal = base_deal(AmortizationType::ANN);
    const auto flows = flows_of(deal);
    const double pmt = annuity_payment(1200000.0, 0.12, 4);
    ASSERT_EQ(flows.size(), 8u);
    EXPECT_NEAR(flows[0].amount + flows[1].amount, pmt, 1e-9);
    EXPECT_NEAR(flows[2].amount + flows[3].amount, pmt, 1e-9);
    EXPECT_NEAR(total(flows, FlowType::Principal), 1200000.0, 1e-6);
}

TEST(Annuity, PrincipalNeverNegativeWhenInterestExceedsPayment) {
    Deal deal = deal_one();
    deal.rate = 0.9;
    deal.principal_rule = monthly(from_ymd(2022, 8, 22));
    deal.start_date = from_ymd(2020, 6, 22);
    deal.interest_rule = monthly(from_ymd(2022, 8, 22));
    DealArena arena;
    for (const CashFlow& flow : flows_of(deal, arena)) {
        EXPECT_GE(flow.amount, 0.0);
    }
}

TEST(Simple, ExplicitPrincipalListIsUsedAsGiven) {
    Deal deal = base_deal(AmortizationType::SIMPLE);
    deal.principal_rule = PaymentRule{};
    deal.principal_schedule = {0, 2};
    DealArena arena;
    arena.principal_items = {{from_ymd(2022, 9, 30), 200000.0}, {from_ymd(2022, 12, 31), 1000000.0}};
    const auto principal = only(flows_of(deal, arena), FlowType::Principal);
    ASSERT_EQ(principal.size(), 2u);
    EXPECT_DOUBLE_EQ(principal[0].amount, 200000.0);
    EXPECT_DOUBLE_EQ(principal[1].amount, 1000000.0);
}

TEST(Simple, PastItemsDoNotCountAgainstBalance) {
    Deal deal = base_deal(AmortizationType::SIMPLE);
    deal.principal_rule = PaymentRule{};
    deal.principal_schedule = {0, 2};
    DealArena arena;
    arena.principal_items = {{from_ymd(2022, 3, 31), 500000.0}, {from_ymd(2022, 12, 31), 1200000.0}};
    EXPECT_EQ(only(flows_of(deal, arena), FlowType::Principal).size(), 1u);
}

TEST(Simple, RejectsScheduleLargerThanBalance) {
    Deal deal = base_deal(AmortizationType::SIMPLE);
    deal.principal_rule = PaymentRule{};
    deal.principal_schedule = {0, 1};
    DealArena arena;
    arena.principal_items = {{from_ymd(2022, 12, 31), 1300000.0}};
    std::vector<CashFlow> flows;
    EXPECT_EQ(generate_cashflows(deal, arena, Policy{}, flows), ErrorCode::ScheduleSumMismatch);
}

TEST(Simple, RejectsScheduleSmallerThanBalance) {
    Deal deal = base_deal(AmortizationType::SIMPLE);
    deal.principal_rule = PaymentRule{};
    deal.principal_schedule = {0, 1};
    DealArena arena;
    arena.principal_items = {{from_ymd(2022, 12, 31), 100000.0}};
    std::vector<CashFlow> flows;
    EXPECT_EQ(generate_cashflows(deal, arena, Policy{}, flows), ErrorCode::ScheduleEndsEarly);
}

TEST(Simple, KopeckRoundingInScheduleIsAccepted) {
    Deal deal = base_deal(AmortizationType::SIMPLE);
    deal.principal_rule = PaymentRule{};
    deal.outstanding_principal = 224204.19;
    deal.principal_schedule = {0, 2};
    DealArena arena;
    arena.principal_items = {{from_ymd(2022, 10, 31), 100000.0}, {from_ymd(2022, 12, 31), 124204.18}};
    std::vector<CashFlow> flows;
    DealSummary summary;
    ASSERT_EQ(generate_cashflows(deal, arena, Policy{}, flows, &summary), ErrorCode::Ok);
    EXPECT_NEAR(summary.total_principal, 224204.19, 1e-6);
    EXPECT_EQ(summary.closing_balance, 0.0);
}

TEST(Simple, InterestAfterLastListedDateIsPaidWithFinalEvent) {
    Deal deal = base_deal(AmortizationType::SIMPLE);
    deal.principal_rule = end_date();
    deal.interest_rule = PaymentRule{};
    deal.interest_schedule = {0, 2};
    DealArena arena;
    arena.interest_dates = {from_ymd(2022, 8, 15), from_ymd(2022, 10, 15)};
    const auto interest = only(flows_of(deal, arena), FlowType::Interest);
    ASSERT_EQ(interest.size(), 2u);
    EXPECT_EQ(interest[1].date, deal.maturity_date);
}

TEST(Rate, CapAndFloorApplyToFixedDeals) {
    Deal deal = base_deal(AmortizationType::RGM);
    deal.day_count = DayCount::Act365;
    deal.cap = 0.10;
    const auto interest = only(flows_of(deal), FlowType::Interest);
    EXPECT_DOUBLE_EQ(interest[0].rate_used, 0.10);
}

TEST(Float, IndicatorPlusSpreadByDefault) {
    Deal deal = base_deal(AmortizationType::RGM);
    deal.rate_type = RateType::Float;
    deal.rate = 0.05;
    deal.indicator_rate = 0.08;
    EXPECT_NEAR(only(flows_of(deal), FlowType::Interest)[0].rate_used, 0.13, 1e-15);

    Policy policy;
    policy.float_rate_is_spread = false;
    EXPECT_NEAR(only(flows_of(deal, {}, policy), FlowType::Interest)[0].rate_used, 0.08, 1e-15);
}

TEST(Float, CapLimitsTotalRate) {
    Deal deal = base_deal(AmortizationType::RGM);
    deal.rate_type = RateType::Float;
    deal.rate = 0.05;
    deal.indicator_rate = 0.08;
    deal.cap = 0.10;
    EXPECT_DOUBLE_EQ(only(flows_of(deal), FlowType::Interest)[0].rate_used, 0.10);
}

TEST(Float, MissingIndicatorIsAnError) {
    Deal deal = base_deal(AmortizationType::RGM);
    deal.rate_type = RateType::Float;
    std::vector<CashFlow> flows;
    EXPECT_EQ(generate_cashflows(deal, {}, Policy{}, flows), ErrorCode::MissingRate);
}

TEST(Float, ReportDateFixingStaysForFuturePayments) {
    Deal deal = base_deal(AmortizationType::RGM);
    deal.rate_type = RateType::Float;
    deal.rate = 0.0;
    deal.reset_rule = monthly(deal.start_date);
    DealArena arena;
    arena.indicator_fixings = {{from_ymd(2022, 1, 1), 0.08}, {from_ymd(2022, 10, 1), 0.20}};
    // Значение 01.10.2022 относится к будущему и не должно использоваться.
    for (const CashFlow& flow : only(flows_of(deal, arena), FlowType::Interest)) {
        EXPECT_DOUBLE_EQ(flow.rate_used, 0.08);
    }
}

TEST(Float, HistoricResetInsideFirstPeriodSplitsRate) {
    Deal deal = base_deal(AmortizationType::SIMPLE);
    deal.day_count = DayCount::Act365;
    deal.rate_type = RateType::Float;
    deal.rate = 0.0;
    deal.principal_rule = end_date();
    deal.interest_rule = monthly(from_ymd(2022, 1, 15));
    deal.reset_schedule = {0, 1};
    DealArena arena;
    arena.reset_items = {{from_ymd(2022, 8, 25), std::nullopt}};
    arena.indicator_fixings = {{from_ymd(2022, 1, 1), 0.10}, {from_ymd(2022, 8, 20), 0.20}};
    const auto interest = only(flows_of(deal, arena), FlowType::Interest);
    const double expected = 1200000.0 * 0.10 * 10.0 / 365.0 + 1200000.0 * 0.20 * 21.0 / 365.0;
    EXPECT_NEAR(interest[0].amount, expected, 1e-9);
}

TEST(Float, ContractRateOnResetReplacesComponent) {
    Deal deal = base_deal(AmortizationType::SIMPLE);
    deal.day_count = DayCount::Act365;
    deal.principal_rule = end_date();
    deal.reset_schedule = {0, 1};
    DealArena arena;
    arena.reset_items = {{from_ymd(2022, 9, 30), 0.20}};
    const auto interest = only(flows_of(deal, arena), FlowType::Interest);
    EXPECT_DOUBLE_EQ(interest[0].rate_used, 0.12);
    EXPECT_DOUBLE_EQ(interest[1].rate_used, 0.20);
}

TEST(Float, AnnuityPaymentChangesAfterReset) {
    Deal deal = base_deal(AmortizationType::ANN);
    deal.reset_schedule = {0, 1};
    DealArena arena;
    arena.reset_items = {{from_ymd(2022, 9, 30), 0.24}};
    const auto flows = flows_of(deal, arena);
    EXPECT_NEAR(total(flows, FlowType::Principal), 1200000.0, 1e-6);
    const double first = flows[0].amount + flows[1].amount;
    const double second = flows[2].amount + flows[3].amount;
    EXPECT_GT(second, first);
}

TEST(CurrentInterest, ReportedValueIsKeptEvenWhenZero) {
    Deal deal = base_deal(AmortizationType::RGM);
    deal.accrued_interest = 0.0;
    std::vector<CashFlow> flows;
    DealSummary summary;
    ASSERT_EQ(generate_cashflows(deal, {}, Policy{}, flows, &summary), ErrorCode::Ok);
    EXPECT_EQ(summary.current_interest_source, InterestSource::Reported);
    EXPECT_EQ(summary.current_interest, 0.0);
}

TEST(CurrentInterest, ComputedFromLastInterestDate) {
    Deal deal = base_deal(AmortizationType::RGM);
    deal.day_count = DayCount::Act365;
    deal.interest_rule = monthly(from_ymd(2022, 1, 21));
    std::vector<CashFlow> flows;
    DealSummary summary;
    ASSERT_EQ(generate_cashflows(deal, {}, Policy{}, flows, &summary), ErrorCode::Ok);
    EXPECT_EQ(summary.current_interest_source, InterestSource::Computed);
    EXPECT_NEAR(*summary.current_interest, 1200000.0 * 0.12 * 10.0 / 365.0, 1e-9);
}

TEST(CurrentInterest, DoesNotChangeFirstCoupon) {
    Deal deal = base_deal(AmortizationType::RGM);
    deal.day_count = DayCount::Act365;
    const auto without = only(flows_of(deal), FlowType::Interest);
    deal.accrued_interest = 1000.0;
    const auto with = only(flows_of(deal), FlowType::Interest);
    EXPECT_DOUBLE_EQ(without[0].amount, with[0].amount);
}

TEST(CurrentInterest, UnavailableWhenDealStartsAfterReport) {
    Deal deal = base_deal(AmortizationType::SIMPLE);
    deal.start_date = from_ymd(2022, 9, 10);
    deal.interest_rule = monthly(deal.start_date);
    deal.principal_rule = end_date();
    std::vector<CashFlow> flows;
    DealSummary summary;
    ASSERT_EQ(generate_cashflows(deal, {}, Policy{}, flows, &summary), ErrorCode::Ok);
    EXPECT_EQ(summary.current_interest_source, InterestSource::Unavailable);
    EXPECT_FALSE(summary.current_interest.has_value());
}
