#include "prepayment_cli.hpp"

#include <set>

#include "cfcore/csv_io.hpp"
#include "model_input.hpp"

namespace prepayment_cli {

namespace {

double parse_bvdpk(std::string_view text) {
    const double result = model_input::number<double>(text, "bvdpk");
    if (result < 0 || result > 1) throw std::runtime_error("bvdpk must be in [0, 1] per 30 days");
    return result;
}

}  // namespace

Configuration read_configuration(const Arguments& args) {
    const auto scenario = cfcore::prepayment_scenario(args.scenario.value_or("base"));
    if (!scenario) throw std::runtime_error("unknown prepayment scenario");
    const std::string source = args.source.value_or("demonstration");
    if (source != "demonstration" && source != "bank") {
        throw std::runtime_error("--bvdpk-source must be demonstration or bank");
    }
    if (args.bvdpk.has_value() == !args.bvdpk_file.empty()) {
        throw std::runtime_error("provide exactly one of --bvdpk or --bvdpk-file");
    }
    if (source == "bank" && args.bvdpk_file.empty()) {
        throw std::runtime_error("bank source requires --bvdpk-file");
    }
    if (args.eligibility_file.empty()) {
        throw std::runtime_error("bvdpk requires --prepayment-eligible");
    }
    std::set<int32_t> eligible;
    model_input::csv(args.eligibility_file, "portfolio_key,recast", 2, [&](const auto& fields) {
        const auto key = model_input::number<int32_t>(fields[0], "portfolio_key");
        if (fields[1] != "proportional" || !eligible.insert(key).second) {
            throw std::runtime_error("invalid recast rule or duplicate portfolio_key");
        }
    });
    if (eligible.empty()) throw std::runtime_error("no portfolios explicitly eligible for prepayment");

    Configuration config{{}, *scenario, source == "demonstration"};
    if (args.bvdpk) {
        const double rate = parse_bvdpk(*args.bvdpk);
        for (const auto key : eligible) {
            config.rates.emplace(key, rate);
        }
    } else {
        model_input::csv(args.bvdpk_file, "portfolio_key,bvdpk", 2, [&](const auto& fields) {
            const auto key = model_input::number<int32_t>(fields[0], "portfolio_key");
            if (!eligible.contains(key) || !config.rates.emplace(key, parse_bvdpk(fields[1])).second) {
                throw std::runtime_error("ineligible or duplicate portfolio_key");
            }
        });
        if (config.rates.size() != eligible.size()) {
            throw std::runtime_error("missing bvdpk for eligible portfolio");
        }
    }
    return config;
}

void write_bucket_header(FILE* file) {
    std::fprintf(file, "portfolio_key,currency,side,report_date,bucket,interval_days,opening_balance,"
                 "scheduled_principal_cf,prepayment_cf,principal_cf,interest_cf,closing_balance\n");
}

void write_buckets(FILE* file, int32_t key, cfcore::Date report_date,
                   const std::array<cfcore::BehavioralBucket, cfcore::kBucketCount>& buckets) {
    const std::string report = cfcore::format_date(report_date);
    for (int i = 0; i < cfcore::kBucketCount; ++i) {
        const auto& row = buckets[i];
        std::fprintf(file, "%d,RUB,ASSET,%s,%d,%d,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g\n",
                     key, report.c_str(), i, row.interval_days, row.opening_balance,
                     row.scheduled_principal, row.prepayment, row.scheduled_principal + row.prepayment,
                     row.interest, row.closing_balance);
    }
}

}  // namespace prepayment_cli
