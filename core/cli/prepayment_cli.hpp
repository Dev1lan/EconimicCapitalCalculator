#pragma once

#include <cstdio>
#include <optional>
#include <string>

#include "cfcore/prepayment.hpp"

namespace prepayment_cli {

struct Arguments {
    bool enabled = false;
    std::optional<std::string> bvdpk;
    std::string bvdpk_file;
    std::string eligibility_file;
    std::optional<std::string> scenario;
    std::optional<std::string> source;
};

struct Configuration {
    cfcore::PrepaymentRates rates;
    cfcore::PrepaymentScenario scenario;
    bool demonstration;
};

Configuration read_configuration(const Arguments& args);
void write_bucket_header(FILE* file);
void write_buckets(FILE* file, int32_t key, cfcore::Date report_date,
                   const std::array<cfcore::BehavioralBucket, cfcore::kBucketCount>& buckets);

}  // namespace prepayment_cli
