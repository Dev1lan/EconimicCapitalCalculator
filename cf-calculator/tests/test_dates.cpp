#include "test_helpers.hpp"

namespace cf::testing {

TEST(DateUtilsTest, Date32RoundTripAndParsing) {
    EXPECT_EQ(dates::to_date32(1970, 1, 1), 0);

    const int32_t rep = D("2022-08-31");
    EXPECT_EQ(rep, D("31.08.2022"));
    EXPECT_EQ(dates::format_iso(rep), "2022-08-31");
}

TEST(DateUtilsTest, AddMonthsPinnedAndEndOfMonth) {
    EXPECT_EQ(dates::format_iso(dates::add_months_pinned(D("2023-01-31"), 1, 31)), "2023-02-28");
    EXPECT_EQ(dates::format_iso(dates::add_months_pinned(D("2024-01-31"), 1, 31)), "2024-02-29");

    const int32_t anchor = D("2022-08-15");
    EXPECT_EQ(dates::format_iso(dates::add_months_pinned(anchor, 1, -1)), "2022-09-30");
    EXPECT_EQ(dates::format_iso(dates::add_months_pinned(anchor, 2, -1)), "2022-10-31");
    EXPECT_EQ(dates::format_iso(dates::add_months_pinned(anchor, 6, -1)), "2023-02-28");
}

TEST(DateUtilsTest, YearFractionConventions) {
    const double yf_sep = dates::year_fraction(D("2022-08-31"), D("2022-09-22"), DayCount::ACT_ACT_ISDA);
    EXPECT_NEAR(yf_sep, 22.0 / 365.0, 1e-9);
    EXPECT_NEAR(yf_sep, 0.060274, 1e-6);

    const double yf_cross = dates::year_fraction(D("2023-12-22"), D("2024-01-22"), DayCount::ACT_ACT_ISDA);
    const double expected_isda = (10.0 / 365.0) + (21.0 / 366.0);
    EXPECT_NEAR(yf_cross, expected_isda, 1e-9);

    const double yf_365 = dates::year_fraction(D("2023-12-22"), D("2024-01-22"), DayCount::ACT_365);
    EXPECT_NEAR(yf_365, 31.0 / 365.0, 1e-9);
}

} // namespace cf::testing
