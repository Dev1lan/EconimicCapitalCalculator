#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "cfcore/date.hpp"

using namespace cfcore;

namespace {

std::vector<std::vector<int>> read_int_csv(const std::string& name) {
    std::ifstream file(std::string(CFCORE_TEST_DATA_DIR) + "/" + name);
    std::vector<std::vector<int>> rows;
    std::string line;
    std::getline(file, line);  // заголовок
    while (std::getline(file, line)) {
        std::stringstream stream(line);
        std::vector<int> row;
        std::string cell;
        while (std::getline(stream, cell, ',')) {
            row.push_back(std::stoi(cell));
        }
        rows.push_back(row);
    }
    return rows;
}

}  // namespace

TEST(Date, EpochIsZero) {
    EXPECT_EQ(from_ymd(1970, 1, 1), 0);
    EXPECT_EQ(from_ymd(1969, 12, 31), -1);
}

TEST(Date, ReportDateOfTestDeals) {
    EXPECT_EQ(from_ymd(2022, 8, 31), 19235);
}

TEST(Date, EveryDayRoundTripsAndMovesForwardByOne) {
    YearMonthDay previous = to_ymd(from_ymd(1900, 1, 1) - 1);
    for (Date d = from_ymd(1900, 1, 1); d <= from_ymd(2200, 1, 1); ++d) {
        const YearMonthDay ymd = to_ymd(d);
        ASSERT_EQ(from_ymd(ymd.year, ymd.month, ymd.day), d);
        const bool next_day = ymd.day == previous.day + 1 && ymd.month == previous.month;
        const bool next_month = ymd.day == 1 && previous.day == days_in_month(previous.year, previous.month);
        ASSERT_TRUE(next_day || next_month) << d;
        previous = ymd;
    }
}

TEST(Date, MatchesPythonDatetime) {
    const auto rows = read_int_csv("dates.csv");
    ASSERT_GT(rows.size(), 5000u);
    for (const auto& row : rows) {
        const YearMonthDay ymd = to_ymd(row[0]);
        ASSERT_EQ(ymd.year, row[1]);
        ASSERT_EQ(ymd.month, row[2]);
        ASSERT_EQ(ymd.day, row[3]);
        ASSERT_EQ(from_ymd(row[1], row[2], row[3]), row[0]);
    }
}

TEST(Date, LeapYears) {
    EXPECT_TRUE(is_leap_year(2024));
    EXPECT_TRUE(is_leap_year(2000));
    EXPECT_FALSE(is_leap_year(1900));
    EXPECT_FALSE(is_leap_year(2023));
    EXPECT_EQ(days_in_month(2024, 2), 29);
    EXPECT_EQ(days_in_month(2023, 2), 28);
}

TEST(AddMonths, MatchesPythonRelativedelta) {
    const auto rows = read_int_csv("add_months.csv");
    ASSERT_EQ(rows.size(), 5000u);
    for (const auto& row : rows) {
        const Date result = row[2] == 0 ? add_months(row[0], row[1]) : add_months(row[0], row[1], row[2]);
        ASSERT_EQ(result, row[3]) << "date " << row[0] << " months " << row[1] << " day " << row[2];
    }
}

TEST(AddMonths, ClipsToEndOfMonth) {
    EXPECT_EQ(add_months(from_ymd(2022, 1, 31), 1), from_ymd(2022, 2, 28));
    EXPECT_EQ(add_months(from_ymd(2024, 1, 31), 1), from_ymd(2024, 2, 29));
}

TEST(AddMonths, DoesNotDriftWhenCountedFromStart) {
    const Date start = from_ymd(2022, 1, 31);
    EXPECT_EQ(add_months(start, 2), from_ymd(2022, 3, 31));
}

TEST(AddMonths, CrossesYearInBothDirections) {
    EXPECT_EQ(add_months(from_ymd(2022, 12, 15), 1), from_ymd(2023, 1, 15));
    EXPECT_EQ(add_months(from_ymd(2023, 1, 15), -1), from_ymd(2022, 12, 15));
}

TEST(AddMonths, UsesRequestedDay) {
    EXPECT_EQ(add_months(from_ymd(2022, 8, 1), 1, 22), from_ymd(2022, 9, 22));
}
