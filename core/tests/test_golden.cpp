#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

#include "test_helpers.hpp"

using namespace cfcore;
using namespace cfcore::testing;

namespace {

struct GoldenRow {
    Date date;
    double principal, interest, balance;
    int bucket;
};

std::vector<std::vector<std::string>> read_csv(const std::string& path) {
    std::ifstream file(path);
    std::vector<std::vector<std::string>> rows;
    std::string line;
    std::getline(file, line);
    while (std::getline(file, line)) {
        std::stringstream stream(line);
        std::vector<std::string> row;
        std::string cell;
        while (std::getline(stream, cell, ',')) {
            row.push_back(cell);
        }
        rows.push_back(row);
    }
    return rows;
}

std::vector<GoldenRow> golden_rows() {
    std::vector<GoldenRow> result;
    for (const auto& row : read_csv(CFCORE_GOLDEN_DIR "/deal1_flows.csv")) {
        const int y = std::stoi(row[0].substr(0, 4));
        const int m = std::stoi(row[0].substr(5, 2));
        const int d = std::stoi(row[0].substr(8, 2));
        result.push_back({from_ymd(y, m, d), std::stod(row[1]), std::stod(row[2]), std::stod(row[3]),
                          std::stoi(row[4])});
    }
    return result;
}

}  // namespace

// Режим Excel: каждая из 334 строк листа «1 сделка».
TEST(Golden, DealOneMatchesSheetRowByRow) {
    const auto golden = golden_rows();
    ASSERT_EQ(golden.size(), 334u);

    std::vector<CashFlow> flows;
    ASSERT_EQ(generate_cashflows(deal_one(), {}, Policy::excel_etalon(), flows), ErrorCode::Ok);
    const auto interest = only(flows, FlowType::Interest);
    const auto principal = only(flows, FlowType::Principal);
    ASSERT_EQ(interest.size(), 334u);
    ASSERT_EQ(principal.size(), 334u);

    const Bucketizer bucketizer(deal_one().report_date);
    double balance = deal_one().outstanding_principal;
    for (size_t i = 0; i < golden.size(); ++i) {
        balance -= principal[i].amount;
        ASSERT_EQ(interest[i].date, golden[i].date) << i;
        ASSERT_EQ(principal[i].date, golden[i].date) << i;
        ASSERT_NEAR(interest[i].amount, golden[i].interest, 1e-6) << i;
        ASSERT_NEAR(principal[i].amount, golden[i].principal, 1e-6) << i;
        ASSERT_NEAR(balance, golden[i].balance, 1e-6) << i;
        ASSERT_EQ(bucketizer.bucket(golden[i].date), golden[i].bucket) << i;
    }
    EXPECT_NEAR(balance, 15420.45, 0.01);
}

// Дисконт-факторы только для контроля в тесте, ядро про них не знает.
TEST(Golden, DealOneBucketsReproduceSheetNpv) {
    const Deal deal = deal_one();
    std::vector<CashFlow> flows;
    ASSERT_EQ(generate_cashflows(deal, {}, Policy::excel_etalon(), flows), ErrorCode::Ok);
    Accumulator accumulator;
    aggregate(deal, flows, Bucketizer(deal.report_date), accumulator);
    const auto totals = accumulator.totals();
    ASSERT_EQ(totals.size(), 1u);
    const BucketTotals& vector = totals.begin()->second;

    double npv_base = 0.0, npv_stress = 0.0;
    for (const auto& row : read_csv(CFCORE_GOLDEN_DIR "/deal1_df.csv")) {
        const int k = std::stoi(row[0]);
        const double flow = vector.principal[k] + vector.interest[k];
        npv_base += flow * std::stod(row[2]);
        npv_stress += flow * std::stod(row[3]);
    }
    EXPECT_NEAR(npv_base, 2056999.4971249553, 1e-6);
    EXPECT_NEAR(npv_stress, 1892477.0845143073, 1e-6);
}

// Базовый режим: те же даты, но долг гасится полностью.
TEST(Golden, DealOneBaseModeRepaysEverything) {
    std::vector<CashFlow> flows;
    DealSummary summary;
    ASSERT_EQ(generate_cashflows(deal_one(), {}, Policy{}, flows, &summary), ErrorCode::Ok);
    EXPECT_NEAR(summary.total_principal, 3646037.89, 1e-6);
    EXPECT_EQ(summary.closing_balance, 0.0);
    EXPECT_EQ(only(flows, FlowType::Principal).size(), 334u);
}
