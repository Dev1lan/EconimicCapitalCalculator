#pragma once

#include <map>
#include <span>
#include <utility>

#include "cfcore/event.hpp"
#include "cfcore/policy.hpp"

namespace cfcore {

// A present row with no amount confirms that the coupon amount is undetermined.
using RiskCoupons = std::map<std::pair<int64_t, Date>, std::optional<double>>;

enum class RiskRule {
    FixedPaymentDate,
    FloatPrincipalEarlierDate,
    FloatKnownCoupon,
    FloatContractualPremium,
    MissingPlannedRepricing,
    InvalidPlannedRepricing,
    MissingCouponStatus,
    PremiumUnresolved,
};

const char* to_string(RiskRule rule);

struct RiskAllocation {
    int64_t instrument_id;
    int32_t portfolio_key;
    Side side;
    Date report_date;
    Date payment_date;
    FlowType type;
    double original_amount;
    std::optional<Date> next_planned_repricing;
    std::optional<Date> risk_date;
    double risk_amount = 0.0;
    RiskRule rule = RiskRule::FixedPaymentDate;
    const char* coupon_status = "";
};

void allocate_risk(const Deal& deal, const DealArena& arena, const Policy& policy,
                   std::span<const CashFlow> flows, std::span<const EventDetail> trace,
                   const RiskCoupons& coupons, std::vector<RiskAllocation>& output);

}  // namespace cfcore

