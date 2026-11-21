#include "cfcore/prepayment.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "cfcore/engine.hpp"

namespace cfcore {

std::optional<PrepaymentScenario> prepayment_scenario(std::string_view name) {
    // Bank of Russia 8-MR, appendix table 2, column 3 (loans).
    constexpr PrepaymentScenario scenarios[] = {
        {"base", 1.0}, {"parallel-up", 0.8}, {"parallel-down", 1.2}, {"steepener", 0.8},
        {"flattener", 1.2}, {"short-up", 0.8}, {"short-down", 1.2},
    };
    for (const auto& scenario : scenarios) {
        if (scenario.name == name) return scenario;
    }
    return std::nullopt;
}

void PrepaymentPortfolio::add(const Deal& deal, std::span<const CashFlow> flows,
                              double closing_balance) {
    if (error_) return;
    if (deal.side != Side::Asset || deal.rate_type != RateType::Fixed ||
        deal.start_date > deal.report_date || deal.cap || deal.floor ||
        deal.reset_rule.kind != PaymentRule::Kind::None || deal.reset_schedule.length ||
        deal.final_rate_schedule.length) {
        error_ = "UNSUPPORTED_CONTRACT";
        return;
    }
    if (std::abs(closing_balance) > deal.outstanding_principal * 1e-12) {
        error_ = "CONTRACT_RESIDUAL";
        return;
    }
    if (has_deals_ && report_date_ != deal.report_date) {
        error_ = "MIXED_REPORT_DATES";
        return;
    }
    report_date_ = deal.report_date;
    maturity_ = has_deals_ ? std::max(maturity_, deal.maturity_date) : deal.maturity_date;
    has_deals_ = true;
    principal_.add(deal.outstanding_principal);
    const Bucketizer bucketizer(report_date_);
    for (const CashFlow& flow : flows) {
        if (flow.date <= report_date_ || flow.date > deal.maturity_date || !std::isfinite(flow.amount)) {
            error_ = "UNSUPPORTED_PAYMENT";
            return;
        }
        auto& sums = flow.type == FlowType::Principal ? scheduled_principal_ : scheduled_interest_;
        sums[bucketizer.bucket(flow.date)].add(flow.amount);
    }
}

void PrepaymentPortfolio::merge(const PrepaymentPortfolio& other) {
    if (error_) return;
    if (other.error_) {
        error_ = other.error_;
        return;
    }
    if (!other.has_deals_) return;
    if (has_deals_ && report_date_ != other.report_date_) {
        error_ = "MIXED_REPORT_DATES";
        return;
    }
    report_date_ = other.report_date_;
    maturity_ = has_deals_ ? std::max(maturity_, other.maturity_) : other.maturity_;
    has_deals_ = true;
    principal_.merge(other.principal_);
    for (int i = 0; i < kBucketCount; ++i) {
        scheduled_principal_[i].merge(other.scheduled_principal_[i]);
        scheduled_interest_[i].merge(other.scheduled_interest_[i]);
    }
}

std::array<BehavioralBucket, kBucketCount> PrepaymentPortfolio::project(double scenario_rate) const {
    if (error_ || !has_deals_ || !std::isfinite(scenario_rate) || scenario_rate < 0 || scenario_rate > 1) {
        throw std::invalid_argument("invalid portfolio or prepayment scenario rate");
    }

    // Sum remaining scheduled debt from the end to avoid subtraction of nearly equal balances.
    std::array<double, kBucketCount + 1> remaining{};
    for (int i = kBucketCount - 1; i >= 0; --i) {
        remaining[i] = remaining[i + 1] + scheduled_principal_[i].value();
    }
    if (!std::isfinite(remaining[0]) ||
        std::abs(remaining[0] - principal_.value()) > principal_.value() * 1e-12) {
        throw std::runtime_error("contract principal does not reconcile");
    }

    std::array<BehavioralBucket, kBucketCount> result{};
    const Bucketizer bucketizer(report_date_);
    Date lower = report_date_;
    double scale = 1.0;
    double opening = remaining[0];
    for (int i = 0; i < kBucketCount; ++i) {
        const Date upper = bucketizer.upper_bound(i).value_or(std::max(lower, maturity_));
        auto& bucket = result[i];
        bucket.interval_days = upper - lower;
        lower = upper;
        bucket.opening_balance = opening;
        const double scheduled = scheduled_principal_[i].value();
        bucket.scheduled_principal = remaining[i + 1] == 0 && scheduled > 0
                                     ? opening : std::min(opening, scheduled * scale);
        bucket.interest = scheduled_interest_[i].value() * scale;
        if (!std::isfinite(bucket.interest)) throw std::runtime_error("portfolio interest overflow");
        const double available = opening - bucket.scheduled_principal;
        bucket.prepayment = std::min(available, (scenario_rate * (bucket.interval_days / 30.0)) * opening);
        bucket.closing_balance = available - bucket.prepayment;
        if (bucket.prepayment > 0) {
            scale = remaining[i + 1] > 0 ? bucket.closing_balance / remaining[i + 1] : 0.0;
        }
        opening = bucket.closing_balance;
    }
    return result;
}

}  // namespace cfcore
