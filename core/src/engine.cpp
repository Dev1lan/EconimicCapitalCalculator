#include "cfcore/engine.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <thread>

#include "cfcore/date.hpp"

#ifdef __FAST_MATH__
#error "cfcore нельзя собирать с -ffast-math: ломается суммирование Ньюмана"
#endif

namespace cfcore {

namespace {

// Верхние границы бакетов 1..17 в месяцах от даты отчёта (включительно).
constexpr int kUpperMonths[kBucketCount - 2] = {1,  3,  6,  9,  12, 18,  24,  36, 48,
                                                60, 72, 84, 96, 108, 120, 180, 240};

constexpr size_t kChunkSize = 1024;

struct ChunkResult {
    Accumulator accumulator;
    std::vector<CashFlow> flows;
    std::vector<DealSummary> summaries;
    std::vector<DealError> errors;
};

void process_chunk(std::span<const Deal> deals, const DealArena& arena, const Policy& policy,
                   bool keep_flows, ChunkResult& result) {
    std::vector<CashFlow> flows;
    Bucketizer bucketizer(deals.front().report_date);
    for (const Deal& deal : deals) {
        flows.clear();
        DealSummary summary;
        const ErrorCode code = generate_cashflows(deal, arena, policy, flows, &summary);
        if (code != ErrorCode::Ok) {
            result.errors.push_back({deal.instrument_id, code});
            continue;
        }
        if (bucketizer.report_date() != deal.report_date) {
            bucketizer = Bucketizer(deal.report_date);
        }
        aggregate(deal, flows, bucketizer, result.accumulator);
        result.summaries.push_back(summary);
        if (keep_flows) {
            result.flows.insert(result.flows.end(), flows.begin(), flows.end());
        }
    }
}

}  // namespace

const char* to_string(ErrorCode code) {
    switch (code) {
        case ErrorCode::Ok: return "OK";
        case ErrorCode::Unsupported: return "UNSUPPORTED";
        case ErrorCode::InvalidNumber: return "INVALID_NUMBER";
        case ErrorCode::InvalidDate: return "INVALID_DATE";
        case ErrorCode::InvalidSchedule: return "INVALID_SCHEDULE";
        case ErrorCode::ScheduleSumMismatch: return "SCHEDULE_SUM_MISMATCH";
        case ErrorCode::ScheduleEndsEarly: return "SCHEDULE_ENDS_EARLY";
        case ErrorCode::MissingRate: return "MISSING_RATE";
        case ErrorCode::TooManyEvents: return "TOO_MANY_EVENTS";
        case ErrorCode::NonFiniteResult: return "NON_FINITE_RESULT";
        case ErrorCode::ParseError: return "PARSE_ERROR";
    }
    return "UNKNOWN";
}

const char* to_string(InterestSource source) {
    switch (source) {
        case InterestSource::Reported: return "REPORTED";
        case InterestSource::Computed: return "COMPUTED";
        case InterestSource::Unavailable: return "UNAVAILABLE";
    }
    return "UNKNOWN";
}

Bucketizer::Bucketizer(Date report_date) : report_date_(report_date) {
    for (int k = 0; k < kBucketCount - 2; ++k) {
        upper_[k] = add_months(report_date, kUpperMonths[k]);
    }
}

int Bucketizer::bucket(Date date) const {
    if (date <= report_date_) {
        return 0;
    }
    const Date* end = upper_ + (kBucketCount - 2);
    return static_cast<int>(std::lower_bound(upper_, end, date) - upper_) + 1;
}

void Accumulator::add(const BucketKey& key, std::span<const CashFlow> flows,
                      const Bucketizer& bucketizer) {
    Sums& sums = sums_[key];
    for (const CashFlow& flow : flows) {
        NeumaierSum* target = flow.type == FlowType::Principal ? sums.principal : sums.interest;
        target[bucketizer.bucket(flow.date)].add(flow.amount);
    }
}

void Accumulator::merge(const Accumulator& other) {
    for (const auto& [key, source] : other.sums_) {
        Sums& target = sums_[key];
        for (int k = 0; k < kBucketCount; ++k) {
            target.principal[k].merge(source.principal[k]);
            target.interest[k].merge(source.interest[k]);
        }
    }
}

std::map<BucketKey, BucketTotals> Accumulator::totals() const {
    std::map<BucketKey, BucketTotals> result;
    for (const auto& [key, sums] : sums_) {
        BucketTotals& totals = result[key];
        for (int k = 0; k < kBucketCount; ++k) {
            totals.principal[k] = sums.principal[k].value();
            totals.interest[k] = sums.interest[k].value();
        }
    }
    return result;
}

void aggregate(const Deal& deal, std::span<const CashFlow> flows, const Bucketizer& bucketizer,
               Accumulator& accumulator) {
    accumulator.add({deal.portfolio_key, deal.currency, deal.side}, flows, bucketizer);
}

PortfolioResult run_portfolio(std::span<const Deal> deals, const DealArena& arena,
                              const Policy& policy, const RunOptions& options) {
    const size_t chunk_count = (deals.size() + kChunkSize - 1) / kChunkSize;
    std::vector<ChunkResult> chunks(chunk_count);

    std::atomic<size_t> next{0};
    std::exception_ptr failure;
    std::mutex failure_mutex;

    const auto worker = [&] {
        try {
            for (size_t c = next++; c < chunk_count; c = next++) {
                const size_t first = c * kChunkSize;
                const size_t count = std::min(kChunkSize, deals.size() - first);
                process_chunk(deals.subspan(first, count), arena, policy, options.keep_flows, chunks[c]);
            }
        } catch (...) {
            // Системный сбой, например нехватка памяти: передаём его вызывающему коду.
            const std::lock_guard lock(failure_mutex);
            if (!failure) {
                failure = std::current_exception();
            }
            next = chunk_count;
        }
    };

    const size_t thread_count = std::clamp<size_t>(options.threads, 1, std::max<size_t>(chunk_count, 1));
    std::vector<std::thread> threads;
    threads.reserve(thread_count - 1);
    for (size_t t = 1; t < thread_count; ++t) {
        threads.emplace_back(worker);
    }
    worker();
    for (std::thread& thread : threads) {
        thread.join();
    }
    if (failure) {
        std::rethrow_exception(failure);
    }

    PortfolioResult result;
    for (ChunkResult& chunk : chunks) {
        result.accumulator.merge(chunk.accumulator);
        result.flows.insert(result.flows.end(), chunk.flows.begin(), chunk.flows.end());
        result.summaries.insert(result.summaries.end(), chunk.summaries.begin(), chunk.summaries.end());
        result.errors.insert(result.errors.end(), chunk.errors.begin(), chunk.errors.end());
    }
    std::sort(result.errors.begin(), result.errors.end(),
              [](const DealError& a, const DealError& b) { return a.instrument_id < b.instrument_id; });
    return result;
}

}  // namespace cfcore
