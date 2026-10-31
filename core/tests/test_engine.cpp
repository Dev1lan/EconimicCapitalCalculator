#include <gtest/gtest.h>

#include <limits>

#include "test_helpers.hpp"

using namespace cfcore;
using namespace cfcore::testing;

namespace {

std::vector<Deal> mixed_portfolio(int count) {
    std::vector<Deal> deals;
    for (int i = 0; i < count; ++i) {
        const auto type = static_cast<AmortizationType>(i % 2);  // ANN или RGM
        Deal deal = base_deal(type);
        deal.instrument_id = i + 1;
        deal.portfolio_key = i % 3;
        deal.side = i % 5 == 0 ? Side::Liability : Side::Asset;
        deal.outstanding_principal = 1000.0 + i * 37.13;
        deal.rate = 0.05 + (i % 11) * 0.01;
        deals.push_back(deal);
    }
    return deals;
}

}  // namespace

TEST(Bucketizer, UpperBoundIsInclusive) {
    const Date report = from_ymd(2022, 8, 31);
    const Bucketizer b(report);
    EXPECT_EQ(b.bucket(report), 0);
    EXPECT_EQ(b.bucket(report + 1), 1);
    EXPECT_EQ(b.bucket(from_ymd(2022, 9, 30)), 1);   // 31.08 + 1 мес. обрезается до 30.09
    EXPECT_EQ(b.bucket(from_ymd(2022, 10, 1)), 2);
    EXPECT_EQ(b.bucket(from_ymd(2022, 11, 30)), 2);
    EXPECT_EQ(b.bucket(from_ymd(2023, 8, 31)), 5);
    EXPECT_EQ(b.bucket(from_ymd(2023, 9, 1)), 6);
    EXPECT_EQ(b.bucket(from_ymd(2042, 8, 31)), 17);
    EXPECT_EQ(b.bucket(from_ymd(2042, 9, 1)), 18);
}

TEST(Aggregate, SplitsPrincipalAndInterestByBucket) {
    const Deal deal = deal_one();
    const std::vector<CashFlow> flows = {
        {1, from_ymd(2022, 10, 22), FlowType::Principal, 100.0, 0.0, 0.0},
        {1, from_ymd(2022, 11, 22), FlowType::Principal, 50.0, 0.0, 0.0},
        {1, from_ymd(2022, 11, 22), FlowType::Interest, 7.0, 0.0, 0.0},
    };
    Accumulator accumulator;
    aggregate(deal, flows, Bucketizer(deal.report_date), accumulator);
    const BucketTotals vector = accumulator.totals().begin()->second;
    EXPECT_DOUBLE_EQ(vector.principal[2], 150.0);
    EXPECT_DOUBLE_EQ(vector.interest[2], 7.0);
}

TEST(Aggregate, KeysAreSeparatedByPortfolioAndSide) {
    Deal asset = deal_one();
    Deal liability = deal_one();
    liability.side = Side::Liability;
    const std::vector<CashFlow> flows = {{1, from_ymd(2022, 10, 22), FlowType::Principal, 1.0, 0.0, 0.0}};
    Accumulator accumulator;
    const Bucketizer bucketizer(asset.report_date);
    aggregate(asset, flows, bucketizer, accumulator);
    aggregate(liability, flows, bucketizer, accumulator);
    EXPECT_EQ(accumulator.totals().size(), 2u);
}

TEST(Portfolio, ResultDoesNotDependOnThreadCount) {
    const auto deals = mixed_portfolio(5000);
    const auto reference = run_portfolio(deals, {}, Policy{}, {1, true}).accumulator.totals();
    for (const int threads : {2, 16}) {
        const auto result = run_portfolio(deals, {}, Policy{}, {threads, true});
        const auto totals = result.accumulator.totals();
        ASSERT_EQ(totals.size(), reference.size());
        for (const auto& [key, vector] : reference) {
            const BucketTotals& other = totals.at(key);
            for (int k = 0; k < kBucketCount; ++k) {
                // Строгое равенство: это проверка детерминизма, а не точности.
                ASSERT_EQ(vector.principal[k], other.principal[k]);
                ASSERT_EQ(vector.interest[k], other.interest[k]);
            }
        }
    }
}

TEST(Portfolio, FlowsKeepDealOrderForAnyThreadCount) {
    const auto deals = mixed_portfolio(3000);
    const auto flows = run_portfolio(deals, {}, Policy{}, {8, true}).flows;
    for (size_t i = 1; i < flows.size(); ++i) {
        ASSERT_LE(flows[i - 1].instrument_id, flows[i].instrument_id);
    }
}

TEST(Portfolio, BucketTotalsEqualSumOfFlows) {
    const auto deals = mixed_portfolio(2000);
    const auto result = run_portfolio(deals, {}, Policy{}, {4, true});
    double from_flows = 0.0;
    for (const CashFlow& flow : result.flows) {
        from_flows += flow.amount;
    }
    double from_buckets = 0.0;
    for (const auto& [key, vector] : result.accumulator.totals()) {
        for (int k = 0; k < kBucketCount; ++k) {
            from_buckets += vector.principal[k] + vector.interest[k];
        }
    }
    EXPECT_NEAR(from_flows, from_buckets, 1e-6);
}

TEST(Portfolio, EveryDealIsRepaid) {
    const auto deals = mixed_portfolio(2000);
    for (const DealSummary& summary : run_portfolio(deals, {}, Policy{}, {4, false}).summaries) {
        EXPECT_NEAR(summary.total_principal, summary.opening_principal, 1e-6);
        EXPECT_EQ(summary.closing_balance, 0.0);
    }
}

TEST(Portfolio, BadDealIsIsolatedAndDoesNotPoisonBuckets) {
    auto deals = mixed_portfolio(100);
    deals[10].outstanding_principal = std::numeric_limits<double>::quiet_NaN();
    deals[20].maturity_date = deals[20].report_date;
    const auto result = run_portfolio(deals, {}, Policy{}, {4, false});
    ASSERT_EQ(result.errors.size(), 2u);
    EXPECT_EQ(result.errors[0].instrument_id, 11);
    EXPECT_EQ(result.errors[0].code, ErrorCode::InvalidNumber);
    EXPECT_EQ(result.errors[1].code, ErrorCode::InvalidDate);
    EXPECT_EQ(result.summaries.size(), 98u);
    for (const auto& [key, vector] : result.accumulator.totals()) {
        for (int k = 0; k < kBucketCount; ++k) {
            ASSERT_TRUE(std::isfinite(vector.principal[k]));
        }
    }
}

TEST(Portfolio, EmptyInputGivesEmptyResult) {
    const auto result = run_portfolio({}, {}, Policy{}, {4, true});
    EXPECT_TRUE(result.errors.empty());
    EXPECT_TRUE(result.accumulator.totals().empty());
}
