#pragma once

#include <array>
#include <map>
#include <optional>
#include <span>
#include <string_view>

#include "cfcore/event.hpp"
#include "cfcore/finance.hpp"

namespace cfcore {

inline constexpr int kBucketCount = 19;
using PrepaymentRates = std::map<int32_t, double>;

struct PrepaymentScenario {
    std::string_view name;
    double coefficient;
};

std::optional<PrepaymentScenario> prepayment_scenario(std::string_view name);

struct BehavioralBucket {
    int interval_days = 0;
    double opening_balance = 0.0;
    double scheduled_principal = 0.0;
    double prepayment = 0.0;
    double interest = 0.0;
    double closing_balance = 0.0;
};

// One explicitly approved, homogeneous portfolio, with one report date.
class PrepaymentPortfolio {
public:
    void add(const Deal& deal, std::span<const CashFlow> flows, double closing_balance);
    void merge(const PrepaymentPortfolio& other);

    const char* error() const { return error_; }
    Date report_date() const { return report_date_; }
    std::array<BehavioralBucket, kBucketCount> project(double scenario_rate) const;

private:
    bool has_deals_ = false;
    Date report_date_ = 0;
    Date maturity_ = 0;
    const char* error_ = nullptr;
    NeumaierSum principal_;
    std::array<NeumaierSum, kBucketCount> scheduled_principal_;
    std::array<NeumaierSum, kBucketCount> scheduled_interest_;
};

}  // namespace cfcore
