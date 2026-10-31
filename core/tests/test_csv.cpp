#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

#include "cfcore/csv_io.hpp"
#include "cfcore/date.hpp"

using namespace cfcore;

namespace {

const char kHeader[] =
    "deal_id;report_dttm;discriminator;currency_id;side;portfolio_key;current_principal_amt;"
    "deal_dttm;maturity_dttm;rate_type_cd;interest_rate;base_indicator_rate;cap_limit_interest_rate;"
    "floor_limit_interest_rate;accrued_interest;day_convention_cd;"
    "interest_payment_rule_start_dttm;interest_payment_rule_period_cd;interest_payment_rule_day_num;"
    "regular_payment_rule_start_dttm;regular_payment_rule_period_cd;regular_payment_rule_day_num;"
    "repricing_rule_start_dttm;repricing_rule_period_cd;repricing_rule_day_num;"
    "interest_payment;principal_payment;repricing_dttm_schedule;repricing_dttm_rate_schedule_fixed\n";

const char kSimpleRow[] =
    "16;2022-08-31;SIMPLE;RUB;LIABILITY;3;1000.5;2020-11-23;2023-04-30;FLOAT;0.05;0.08;0.2;0.01;0;"
    "ACT_ACT_ISDA;2020-11-23;1M;7;;ENDDATE;;2022-06-30;2M;-1;2022-09-07 2022-10-07;"
    "2022-12-31,250.5 2023-04-30,750;2022-07-15;2022-08-31,0.05\n";

class TempFile {
public:
    explicit TempFile(const std::string& content) {
        path_ = "cfcore_test_" + std::to_string(getpid()) + "_" + std::to_string(counter_++) + ".csv";
        std::ofstream(path_) << content;
    }
    ~TempFile() { std::remove(path_.c_str()); }
    const std::string& path() const { return path_; }

private:
    static inline int counter_ = 0;
    std::string path_;
};

}  // namespace

TEST(Csv, ReadsAllFieldsOfRow) {
    const TempFile file(std::string(kHeader) + kSimpleRow);
    CsvDealReader reader(file.path());
    std::vector<Deal> deals;
    DealArena arena;
    std::vector<DealError> errors;
    ASSERT_TRUE(reader.next_batch(10, deals, arena, errors));
    ASSERT_EQ(deals.size(), 1u);
    EXPECT_TRUE(errors.empty());

    const Deal& deal = deals[0];
    EXPECT_EQ(deal.instrument_id, 16);
    EXPECT_EQ(deal.portfolio_key, 3);
    EXPECT_EQ(deal.side, Side::Liability);
    EXPECT_EQ(deal.amortization, AmortizationType::SIMPLE);
    EXPECT_EQ(deal.rate_type, RateType::Float);
    EXPECT_EQ(deal.day_count, DayCount::ActActIsda);
    EXPECT_DOUBLE_EQ(deal.outstanding_principal, 1000.5);
    EXPECT_EQ(deal.indicator_rate, 0.08);
    EXPECT_EQ(deal.cap, 0.2);
    EXPECT_EQ(deal.floor, 0.01);
    EXPECT_EQ(deal.accrued_interest, 0.0);
    EXPECT_EQ(deal.interest_rule.kind, PaymentRule::Kind::Months);
    EXPECT_EQ(deal.interest_rule.period, 1);
    EXPECT_EQ(deal.interest_rule.day, 7);
    EXPECT_EQ(deal.principal_rule.kind, PaymentRule::Kind::EndDate);
    EXPECT_EQ(deal.reset_rule.day, -1);

    ASSERT_EQ(deal.interest_schedule.length, 2u);
    EXPECT_EQ(arena.interest_dates[1], from_ymd(2022, 10, 7));
    ASSERT_EQ(deal.principal_schedule.length, 2u);
    EXPECT_EQ(arena.principal_items[0].amount, 250.5);
    ASSERT_EQ(deal.reset_schedule.length, 2u);
    EXPECT_FALSE(arena.reset_items[0].fixed_rate.has_value());
    EXPECT_EQ(arena.reset_items[1].fixed_rate, 0.05);
}

TEST(Csv, EmptyOptionalStaysEmpty) {
    const TempFile file(std::string(kHeader) +
                        "1;2022-08-31;ANN;RUB;ASSET;0;100;2020-01-01;2030-01-01;FIXED;0.1;;;;;ACT_365;"
                        ";;;2020-01-01;1M;;;;;;;;\n");
    CsvDealReader reader(file.path());
    std::vector<Deal> deals;
    DealArena arena;
    std::vector<DealError> errors;
    ASSERT_TRUE(reader.next_batch(10, deals, arena, errors));
    ASSERT_EQ(deals.size(), 1u);
    EXPECT_FALSE(deals[0].accrued_interest.has_value());
    EXPECT_FALSE(deals[0].cap.has_value());
    EXPECT_EQ(deals[0].interest_rule.kind, PaymentRule::Kind::None);
}

TEST(Csv, BadRowBecomesErrorAndFileContinues) {
    const std::string bad =
        "2;2022-99-31;ANN;RUB;ASSET;0;100;2020-01-01;2030-01-01;FIXED;0.1;;;;;ACT_365;"
        ";;;2020-01-01;1M;;;;;;;;\n";
    const TempFile file(std::string(kHeader) + bad + kSimpleRow);
    CsvDealReader reader(file.path());
    std::vector<Deal> deals;
    DealArena arena;
    std::vector<DealError> errors;
    ASSERT_TRUE(reader.next_batch(10, deals, arena, errors));
    EXPECT_EQ(deals.size(), 1u);
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0].instrument_id, 2);
    EXPECT_EQ(errors[0].code, ErrorCode::ParseError);
}

TEST(Csv, UnknownCodeIsAnError) {
    std::string row = kSimpleRow;
    row.replace(row.find("SIMPLE"), 6, "OTHER");
    const TempFile file(std::string(kHeader) + row);
    CsvDealReader reader(file.path());
    std::vector<Deal> deals;
    DealArena arena;
    std::vector<DealError> errors;
    reader.next_batch(10, deals, arena, errors);
    EXPECT_TRUE(deals.empty());
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0].code, ErrorCode::ParseError);
}

TEST(Csv, OtherCurrencyIsUnsupported) {
    std::string row = kSimpleRow;
    row.replace(row.find("RUB"), 3, "USD");
    const TempFile file(std::string(kHeader) + row);
    CsvDealReader reader(file.path());
    std::vector<Deal> deals;
    DealArena arena;
    std::vector<DealError> errors;
    reader.next_batch(10, deals, arena, errors);
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0].code, ErrorCode::Unsupported);
}

TEST(Csv, ReadsFileInBatches) {
    std::string content = kHeader;
    for (int i = 0; i < 5; ++i) {
        content += kSimpleRow;
    }
    const TempFile file(content);
    CsvDealReader reader(file.path());
    std::vector<Deal> deals;
    DealArena arena;
    std::vector<DealError> errors;
    ASSERT_TRUE(reader.next_batch(2, deals, arena, errors));
    EXPECT_EQ(deals.size(), 2u);
    EXPECT_EQ(arena.interest_dates.size(), 4u);
    ASSERT_TRUE(reader.next_batch(2, deals, arena, errors));
    ASSERT_TRUE(reader.next_batch(2, deals, arena, errors));
    EXPECT_EQ(deals.size(), 1u);
    EXPECT_FALSE(reader.next_batch(2, deals, arena, errors));
}

TEST(Csv, AcceptsWindowsLineEndings) {
    std::string content = std::string(kHeader) + kSimpleRow;
    for (size_t pos = content.find('\n'); pos != std::string::npos; pos = content.find('\n', pos + 2)) {
        content.insert(pos, "\r");
    }
    const TempFile file(content);
    CsvDealReader reader(file.path());
    std::vector<Deal> deals;
    DealArena arena;
    std::vector<DealError> errors;
    ASSERT_TRUE(reader.next_batch(10, deals, arena, errors));
    EXPECT_EQ(deals.size(), 1u);
    EXPECT_TRUE(errors.empty());
}

TEST(Csv, MissingColumnStopsRun) {
    const TempFile file("deal_id;report_dttm\n1;2022-08-31\n");
    EXPECT_THROW(CsvDealReader{file.path()}, std::runtime_error);
}

TEST(Date, ParseAndFormatRoundTrip) {
    Date date = 0;
    ASSERT_TRUE(parse_date("2024-02-29", date));
    EXPECT_EQ(format_date(date), "2024-02-29");
    EXPECT_FALSE(parse_date("2023-02-29", date));
    EXPECT_FALSE(parse_date("31.08.2022", date));
}
