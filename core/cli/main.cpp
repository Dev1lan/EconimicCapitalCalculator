#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "cfcore/csv_io.hpp"
#include "cfcore/engine.hpp"

using namespace cfcore;

namespace {

constexpr size_t kBatchSize = 100000;

struct Arguments {
    std::string input;
    std::string output_dir;
    std::string fixings;
    int threads = 0;
    bool excel_etalon = false;
    bool write_flows = true;
    bool round_output = false;
};

void print_usage() {
    std::cout <<
        "Использование: cfcore --input deals.csv --output-dir out [опции]\n"
        "  --fixings FILE    история индикатора (date,value)\n"
        "  --threads N       число потоков, по умолчанию все ядра\n"
        "  --excel-etalon    правила листа \"1 сделка\"\n"
        "  --no-flows        не писать cashflows.csv\n"
        "  --round-money     округлить суммы до копеек при выводе\n";
}

Arguments parse_arguments(int argc, char** argv) {
    Arguments args;
    for (int i = 1; i < argc; ++i) {
        const std::string name = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error("нет значения для " + name);
            }
            return argv[++i];
        };
        if (name == "--input") args.input = value();
        else if (name == "--output-dir") args.output_dir = value();
        else if (name == "--fixings") args.fixings = value();
        else if (name == "--threads") args.threads = std::stoi(value());
        else if (name == "--excel-etalon") args.excel_etalon = true;
        else if (name == "--no-flows") args.write_flows = false;
        else if (name == "--round-money") args.round_output = true;
        else throw std::runtime_error("неизвестная опция " + name);
    }
    if (args.input.empty() || args.output_dir.empty()) {
        throw std::runtime_error("нужны --input и --output-dir");
    }
    return args;
}

// Файл закрывается автоматически, ошибка открытия останавливает запуск.
using File = std::unique_ptr<FILE, int (*)(FILE*)>;

File open_output(const std::filesystem::path& path) {
    File file(std::fopen(path.c_str(), "w"), std::fclose);
    if (!file) {
        throw std::runtime_error("не удалось создать файл: " + path.string());
    }
    return file;
}

const char* side_name(Side side) {
    return side == Side::Asset ? "ASSET" : "LIABILITY";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 1 || std::strcmp(argv[1], "--help") == 0) {
        print_usage();
        return argc == 1 ? 1 : 0;
    }
    try {
        const auto started = std::chrono::steady_clock::now();
        const Arguments args = parse_arguments(argc, argv);
        const int threads = args.threads > 0 ? args.threads
                                             : static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
        const Policy policy = args.excel_etalon ? Policy::excel_etalon() : Policy{};
        const auto money = [&](double value) { return args.round_output ? round_money(value) : value; };

        const std::filesystem::path out_dir(args.output_dir);
        std::filesystem::create_directories(out_dir);

        File flows_file(nullptr, std::fclose);
        if (args.write_flows) {
            flows_file = open_output(out_dir / "cashflows.csv");
            std::fprintf(flows_file.get(),
                         "instrument_id,payment_date,cash_flow_type,amount,rate_used,year_fraction\n");
        }
        File summary_file = open_output(out_dir / "deal_summary.csv");
        std::fprintf(summary_file.get(),
                     "instrument_id,opening_principal,total_principal,total_interest,closing_balance,"
                     "flow_count,current_interest,current_interest_source\n");

        DealArena arena;
        if (!args.fixings.empty()) {
            arena.indicator_fixings = read_fixings(args.fixings);
        }

        CsvDealReader reader(args.input);
        std::vector<Deal> deals;
        std::vector<DealError> errors;
        Accumulator accumulator;
        size_t deals_ok = 0, flow_count = 0;
        RunOptions options;
        options.threads = threads;
        options.keep_flows = args.write_flows;

        // Порция считается, результат записывается, память порции переиспользуется.
        while (reader.next_batch(kBatchSize, deals, arena, errors)) {
            PortfolioResult result = run_portfolio(deals, arena, policy, options);
            accumulator.merge(result.accumulator);
            errors.insert(errors.end(), result.errors.begin(), result.errors.end());
            deals_ok += result.summaries.size();
            for (const CashFlow& flow : result.flows) {
                std::fprintf(flows_file.get(), "%lld,%s,%s,%.10f,%.10f,%.10f\n",
                             static_cast<long long>(flow.instrument_id), format_date(flow.date).c_str(),
                             flow.type == FlowType::Principal ? "PRINCIPAL" : "INTEREST",
                             money(flow.amount), flow.rate_used, flow.year_fraction);
            }
            for (const DealSummary& s : result.summaries) {
                flow_count += s.flow_count;
                std::fprintf(summary_file.get(), "%lld,%.10f,%.10f,%.10f,%.10f,%d,",
                             static_cast<long long>(s.instrument_id), money(s.opening_principal),
                             money(s.total_principal), money(s.total_interest), money(s.closing_balance),
                             s.flow_count);
                if (s.current_interest) {
                    std::fprintf(summary_file.get(), "%.10f", money(*s.current_interest));
                }
                std::fprintf(summary_file.get(), ",%s\n", to_string(s.current_interest_source));
            }
        }

        File buckets_file = open_output(out_dir / "buckets.csv");
        std::fprintf(buckets_file.get(), "portfolio_key,currency,side,bucket,principal_cf,interest_cf\n");
        for (const auto& [key, totals] : accumulator.totals()) {
            for (int k = 0; k < kBucketCount; ++k) {
                if (totals.principal[k] != 0.0 || totals.interest[k] != 0.0) {
                    std::fprintf(buckets_file.get(), "%d,RUB,%s,%d,%.10f,%.10f\n", key.portfolio_key,
                                 side_name(key.side), k, money(totals.principal[k]), money(totals.interest[k]));
                }
            }
        }

        std::sort(errors.begin(), errors.end(),
                  [](const DealError& a, const DealError& b) { return a.instrument_id < b.instrument_id; });
        File errors_file = open_output(out_dir / "errors.csv");
        std::fprintf(errors_file.get(), "instrument_id,error_code\n");
        for (const DealError& error : errors) {
            std::fprintf(errors_file.get(), "%lld,%s\n", static_cast<long long>(error.instrument_id),
                         to_string(error.code));
        }

        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - started;
        File manifest = open_output(out_dir / "run_manifest.json");
        std::fprintf(manifest.get(),
                     "{\n  \"input\": \"%s\",\n  \"mode\": \"%s\",\n  \"threads\": %d,\n"
                     "  \"deals_ok\": %zu,\n  \"deals_failed\": %zu,\n  \"flows\": %zu,\n"
                     "  \"seconds\": %.3f,\n  \"policy\": {\n    \"force_act365\": %s,\n"
                     "    \"close_residual\": %s,\n    \"first_period_from_prev_pay\": %s,\n"
                     "    \"include_payment_on_report\": %s,\n    \"float_rate_is_spread\": %s\n  }\n}\n",
                     args.input.c_str(), args.excel_etalon ? "EXCEL_ETALON" : "BASE", threads, deals_ok,
                     errors.size(), flow_count, elapsed.count(), policy.force_act365 ? "true" : "false",
                     policy.close_residual ? "true" : "false",
                     policy.first_period_from_prev_pay ? "true" : "false",
                     policy.include_payment_on_report ? "true" : "false",
                     policy.float_rate_is_spread ? "true" : "false");

        std::printf("сделок рассчитано: %zu, с ошибкой: %zu, потоков: %zu, потоков ЦП: %d, время: %.3f с\n",
                    deals_ok, errors.size(), flow_count, threads, elapsed.count());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "ошибка: %s\n", error.what());
        return 2;
    }
}
