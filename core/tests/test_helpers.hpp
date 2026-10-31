#pragma once

#include <vector>

#include "cfcore/date.hpp"
#include "cfcore/engine.hpp"

namespace cfcore::testing {

inline PaymentRule monthly(Date start, int day = 0) {
    PaymentRule rule;
    rule.kind = PaymentRule::Kind::Months;
    rule.period = 1;
    rule.start = start;
    rule.day = day;
    return rule;
}

inline PaymentRule end_date() {
    PaymentRule rule;
    rule.kind = PaymentRule::Kind::EndDate;
    return rule;
}

// Кредит 1 200 000 под 12% на дату отчёта 31.08.2022, погашение 31.12.2022.
inline Deal base_deal(AmortizationType amortization) {
    Deal deal;
    deal.instrument_id = 7;
    deal.amortization = amortization;
    deal.outstanding_principal = 1200000.0;
    deal.rate = 0.12;
    deal.report_date = from_ymd(2022, 8, 31);
    deal.start_date = from_ymd(2022, 1, 31);
    deal.maturity_date = from_ymd(2022, 12, 31);
    deal.principal_rule = monthly(deal.start_date);
    deal.interest_rule = monthly(deal.start_date);
    return deal;
}

// Сделка 1 из листа «1 сделка».
inline Deal deal_one() {
    Deal deal;
    deal.instrument_id = 1;
    deal.amortization = AmortizationType::ANN;
    deal.outstanding_principal = 3646037.89;
    deal.rate = 0.075;
    deal.day_count = DayCount::ActActIsda;
    deal.report_date = from_ymd(2022, 8, 31);
    deal.start_date = from_ymd(2020, 6, 22);
    deal.maturity_date = from_ymd(2050, 6, 15);
    deal.principal_rule = monthly(deal.start_date);
    return deal;
}

inline std::vector<CashFlow> only(const std::vector<CashFlow>& flows, FlowType type) {
    std::vector<CashFlow> result;
    for (const CashFlow& flow : flows) {
        if (flow.type == type) {
            result.push_back(flow);
        }
    }
    return result;
}

inline double total(const std::vector<CashFlow>& flows, FlowType type) {
    double sum = 0.0;
    for (const CashFlow& flow : only(flows, type)) {
        sum += flow.amount;
    }
    return sum;
}

}  // namespace cfcore::testing
