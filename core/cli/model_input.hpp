#pragma once

#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace model_input {

template<class T>
T number(std::string_view text, const char* field) {
    T value{};
    const auto end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value);
    if (result.ec != std::errc{} || result.ptr != end || !std::isfinite(value)) {
        throw std::runtime_error(std::string("invalid ") + field + ": " + std::string(text));
    }
    return value;
}

inline std::filesystem::path path(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

// The model inputs use simple comma-separated numeric/ISO/code values, no quotes.
template<class Consume>
void csv(const std::string& filename, std::string_view header, size_t columns, Consume consume) {
    std::ifstream file(path(filename), std::ios::binary);
    std::string line;
    const auto read_line = [&] {
        if (!std::getline(file, line)) {
            return false;
        }
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        return true;
    };
    if (!read_line()) {
        throw std::runtime_error("cannot read " + filename);
    }
    if (line.starts_with("\xef\xbb\xbf")) {
        line.erase(0, 3);
    }
    if (line != header) {
        throw std::runtime_error("invalid CSV header in " + filename);
    }
    for (size_t lineno = 2; read_line(); ++lineno) {
        try {
            std::vector<std::string> fields;
            size_t start = 0;
            for (size_t comma = line.find(','); comma != std::string::npos;
                 comma = line.find(',', start)) {
                fields.push_back(line.substr(start, comma - start));
                start = comma + 1;
            }
            fields.push_back(line.substr(start));
            if (fields.size() != columns) {
                throw std::runtime_error("wrong number of CSV columns");
            }
            consume(fields);
        } catch (const std::runtime_error& error) {
            throw std::runtime_error(filename + ":" + std::to_string(lineno) + ": " + error.what());
        }
    }
    if (file.bad()) {
        throw std::runtime_error("cannot finish reading " + filename);
    }
}

}  // namespace model_input
