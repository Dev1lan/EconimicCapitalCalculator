#include "cfcore/risk.hpp"

#include <algorithm>
#include <cmath>

#include "internal/schedule.hpp"

namespace cfcore {

const char* to_string(RiskRule rule) {
    switch (rule) {
        case RiskRule::FixedPaymentDate: return "FIXED_PAYMENT_DATE";
        case RiskRule::FloatPrincipalEarlierDate: return "FLOAT_PRINCIPAL_EARLIER_DATE";
        case RiskRule::FloatKnownCoupon: return "FLOAT_KNOWN_COUPON";
        case RiskRule::FloatContractualPremium: return "FLOAT_CONTRACTUAL_PREMIUM";
        case RiskRule::MissingPlannedRepricing: return "MISSING_PLANNED_REPRICING";
        case RiskRule::InvalidPlannedRepricing: return "INVALID_PLANNED_REPRICING";
        case RiskRule::MissingCouponStatus: return "MISSING_COUPON_STATUS";
        case RiskRule::PremiumUnresolved: return "PREMIUM_UNRESOLVED";
    }
    return "";
}

void allocate_risk(const Deal& deal, const DealArena& arena, const Policy& policy,
                   std::span<const CashFlow> flows, std::span<const EventDetail> trace,
                   const RiskCoupons& coupons, std::vector<RiskAllocation>& output) {
    const auto allocation = [&](Date date, FlowType type, double original, std::optional<Date> reset) {
        return RiskAllocation{deal.instrument_id, deal.portfolio_key, deal.side, deal.report_date,
                              date, type, original, reset, std::nullopt};
    };
    if (deal.rate_type == RateType::Fixed) {
        for (const CashFlow& flow : flows) {
            auto row = allocation(flow.date, flow.type, flow.amount, std::nullopt);
            row.risk_date = flow.date;
            row.risk_amount = flow.amount;
            output.push_back(row);
        }
        return;
    }

    std::optional<Date> next_reset;
    const auto status = internal::next_planned_reset(deal, arena, next_reset);
    const auto reset_issue = status == ErrorCode::Ok ? RiskRule::MissingPlannedRepricing
                                                     : RiskRule::InvalidPlannedRepricing;
    struct CouponAccrual {
        NeumaierSum original;
        NeumaierSum premium;
    };
    std::map<Date, CouponAccrual> accruals;
    NeumaierSum premium_accrual;
    const auto legacy_spread_override = [&](Span span) {
        const auto resets = std::span(arena.reset_items).subspan(span.offset, span.length);
        return std::any_of(resets.begin(), resets.end(), [](const ResetItem& item) {
            return item.fixed_rate.has_value();
        });
    };
    const bool spread_unresolved = deal.cap || deal.floor || deal.final_rate_schedule.length ||
                                   !policy.float_rate_is_spread ||
                                   legacy_spread_override(deal.reset_schedule);
    for (const EventDetail& event : trace) {
        premium_accrual.add(event.balance_before * (deal.rate * event.year_fraction));
        if (event.type == EventType::PrincipalPayment && event.principal_paid != 0.0) {
            auto row = allocation(event.date, FlowType::Principal, event.principal_paid, next_reset);
            row.rule = reset_issue;
            if (next_reset) {
                row.risk_date = std::min(event.date, *next_reset);
                row.risk_amount = event.principal_paid;
                row.rule = RiskRule::FloatPrincipalEarlierDate;
            }
            output.push_back(row);
        }
        if (event.type == EventType::InterestPayment) {
            auto& accrual = accruals[event.date];
            accrual.original.add(event.interest_paid);
            accrual.premium.merge(premium_accrual);
            premium_accrual = {};
        }
    }
    for (const auto& [date, accrual] : accruals) {
        auto row = allocation(date, FlowType::Interest, accrual.original.value(), next_reset);
        const auto coupon = coupons.find({deal.instrument_id, date});
        if (coupon == coupons.end()) {
            row.rule = RiskRule::MissingCouponStatus;
        } else if (coupon->second) {
            row.coupon_status = "KNOWN";
            row.rule = reset_issue;
            if (next_reset) {
                row.risk_date = next_reset;
                row.risk_amount = *coupon->second;
                row.rule = RiskRule::FloatKnownCoupon;
            }
        } else {
            row.coupon_status = "UNKNOWN";
            row.rule = RiskRule::PremiumUnresolved;
            if (!spread_unresolved && std::isfinite(accrual.premium.value())) {
                row.risk_date = date;
                row.risk_amount = accrual.premium.value();
                row.rule = RiskRule::FloatContractualPremium;
            }
        }
        output.push_back(row);
    }
}

}  // namespace cfcore
