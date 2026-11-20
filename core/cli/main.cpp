#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "cfcore/csv_io.hpp"
#include "cfcore/date.hpp"
#include "cfcore/engine.hpp"

using namespace cfcore;

namespace {

constexpr size_t kBatchSize = 100000;
constexpr size_t kFormatBlock = 65536;  // строк cashflows.csv на поток за один проход
constexpr size_t kMaxRowLength = 160;

struct Arguments {
    std::string input;
    std::string output_dir;
    std::string fixings;
    int threads = 0;
    bool excel_etalon = false;
    bool write_flows = true;
    bool details_only = false;
    bool round_output = false;
};

void print_usage() {
    std::cout <<
        "Использование: cfcore --input deals.csv --output-dir out [опции]\n"
        "  --fixings FILE    история индикатора (date,value)\n"
        "  --threads N       число потоков, по умолчанию все ядра\n"
        "  --excel-etalon    правила листа \"1 сделка\"\n"
        "  --no-flows        не писать cashflows.csv\n"
        "  --details-only    не писать buckets.csv, errors.csv и манифест, без сводки на экране\n"
        "  --round-money     округлить суммы до копеек при выводе\n";
}

Arguments parse_arguments(std::span<const std::string> argv) {
    Arguments args;
    for (size_t i = 1; i < argv.size(); ++i) {
        const std::string& name = argv[i];
        const auto value = [&]() -> const std::string& {
            if (i + 1 >= argv.size()) {
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
        else if (name == "--details-only") args.details_only = true;
        else if (name == "--round-money") args.round_output = true;
        else throw std::runtime_error("неизвестная опция " + name);
    }
    if (args.input.empty() || args.output_dir.empty()) {
        throw std::runtime_error("нужны --input и --output-dir");
    }
    return args;
}

std::string utf8_path(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

std::string json_string(const std::string& text) {
    constexpr char hex[] = "0123456789abcdef";
    std::string quoted = "\"";
    for (unsigned char byte : text) {
        if (byte == '"' || byte == '\\') {
            quoted += '\\';
            quoted += static_cast<char>(byte);
        } else if (byte < 0x20) {
            quoted += "\\u00";
            quoted += hex[byte >> 4];
            quoted += hex[byte & 0x0f];
        } else {
            quoted += static_cast<char>(byte);
        }
    }
    quoted += '"';
    return quoted;
}

// Файл закрывается автоматически, ошибка открытия останавливает запуск.
using File = std::unique_ptr<FILE, int (*)(FILE*)>;

File open_output(const std::filesystem::path& path) {
#ifdef _WIN32
    File file(_wfopen(path.c_str(), L"w"), std::fclose);
#else
    File file(std::fopen(path.c_str(), "w"), std::fclose);
#endif
    if (!file) {
        throw std::runtime_error("не удалось создать файл: " + utf8_path(path));
    }
    return file;
}

char* append_fixed(char* out, double value) {
    return std::to_chars(out, out + 64, value, std::chars_format::fixed, 10).ptr;
}

char* append_digits(char* out, int value, int width) {
    for (int i = width - 1; i >= 0; --i) {
        out[i] = static_cast<char>('0' + value % 10);
        value /= 10;
    }
    return out + width;
}

void append_flow(std::string& text, const CashFlow& flow, bool round_output) {
    char row[kMaxRowLength];
    char* end = std::to_chars(row, row + 32, flow.instrument_id).ptr;
    *end++ = ',';
    const YearMonthDay ymd = to_ymd(flow.date);
    end = append_digits(end, ymd.year, 4);
    *end++ = '-';
    end = append_digits(end, ymd.month, 2);
    *end++ = '-';
    end = append_digits(end, ymd.day, 2);
    const char* type = flow.type == FlowType::Principal ? ",PRINCIPAL," : ",INTEREST,";
    end = std::copy(type, type + std::strlen(type), end);
    end = append_fixed(end, round_output ? round_money(flow.amount) : flow.amount);
    *end++ = ',';
    end = append_fixed(end, flow.rate_used);
    *end++ = ',';
    end = append_fixed(end, flow.year_fraction);
    *end++ = '\n';
    text.append(row, end);
}

// Текст готовят все потоки, а в файл части идут строго по порядку, поэтому
// содержимое не зависит от числа потоков.
void write_flows(FILE* file, std::span<const CashFlow> flows, int threads, bool round_output) {
    std::vector<std::string> parts(threads);
    for (std::string& part : parts) {
        part.reserve(kFormatBlock * kMaxRowLength);
    }
    while (!flows.empty()) {
        std::vector<std::thread> workers;
        int used = 0;
        for (; used < threads && !flows.empty(); ++used) {
            const auto slice = flows.first(std::min(kFormatBlock, flows.size()));
            flows = flows.subspan(slice.size());
            parts[used].clear();
            workers.emplace_back([slice, round_output, &part = parts[used]] {
                for (const CashFlow& flow : slice) {
                    append_flow(part, flow, round_output);
                }
            });
        }
        for (std::thread& worker : workers) {
            worker.join();
        }
        for (int i = 0; i < used; ++i) {
            if (std::fwrite(parts[i].data(), 1, parts[i].size(), file) != parts[i].size()) {
                throw std::runtime_error("не удалось записать cashflows.csv, проверьте место на диске");
            }
        }
    }
}

const char* side_name(Side side) {
    return side == Side::Asset ? "ASSET" : "LIABILITY";
}

int run_cli(std::span<const std::string> argv) {
    if (argv.size() == 1 || argv[1] == "--help") {
        print_usage();
        return argv.size() == 1 ? 1 : 0;
    }
    try {
        const auto started = std::chrono::steady_clock::now();
        const Arguments args = parse_arguments(argv);
        const int threads = args.threads > 0 ? args.threads
                                             : static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
        const Policy policy = args.excel_etalon ? Policy::excel_etalon() : Policy{};
        const auto money = [&](double value) { return args.round_output ? round_money(value) : value; };

        const std::filesystem::path out_dir(std::u8string(args.output_dir.begin(), args.output_dir.end()));
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
            if (args.write_flows) {
                write_flows(flows_file.get(), result.flows, threads, args.round_output);
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

        if (args.details_only) {
            // Без errors.csv пропуск сделок был бы незаметен, поэтому о нём сообщаем всегда.
            if (!errors.empty()) {
                std::map<std::string, size_t> counts;
                for (const DealError& error : errors) {
                    ++counts[to_string(error.code)];
                }
                std::fprintf(stderr, "не посчитано сделок: %zu", errors.size());
                for (const auto& [code, count] : counts) {
                    std::fprintf(stderr, ", %s %zu", code.c_str(), count);
                }
                std::fprintf(stderr, "\n");
            }
            return 0;
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
                     "{\n  \"input\": %s,\n  \"mode\": \"%s\",\n  \"threads\": %d,\n"
                     "  \"deals_ok\": %zu,\n  \"deals_failed\": %zu,\n  \"flows\": %zu,\n"
                     "  \"seconds\": %.3f,\n  \"policy\": {\n    \"force_act365\": %s,\n"
                     "    \"close_residual\": %s,\n    \"first_period_from_prev_pay\": %s,\n"
                     "    \"include_payment_on_report\": %s,\n    \"float_rate_is_spread\": %s\n  }\n}\n",
                     json_string(args.input).c_str(), args.excel_etalon ? "EXCEL_ETALON" : "BASE", threads, deals_ok,
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

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> arguments;
    arguments.reserve(argc);
    for (int i = 0; i < argc; ++i) {
        arguments.push_back(utf8_path(argv[i]));
    }
    return run_cli(arguments);
}
#else
int main(int argc, char** argv) {
    const std::vector<std::string> arguments(argv, argv + argc);
    return run_cli(arguments);
}
#endif
