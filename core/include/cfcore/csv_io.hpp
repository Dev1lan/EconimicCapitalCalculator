#pragma once

#include <cstddef>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "cfcore/engine.hpp"

namespace cfcore {

// Читает нормализованный CSV порциями. Единственное место, где известны имена колонок:
// при переходе на Arrow заменяется этот класс, расчёт не меняется.
class CsvDealReader {
public:
    // Бросает std::runtime_error, если файла нет или не хватает колонки.
    explicit CsvDealReader(const std::string& path);

    // Очищает порцию и читает до max_deals сделок. false: файл закончился и порция пуста.
    // Строки, которые не удалось разобрать, попадают в errors.
    bool next_batch(size_t max_deals, std::vector<Deal>& deals, DealArena& arena,
                    std::vector<DealError>& errors);

private:
    std::ifstream file_;
    std::map<std::string, size_t> columns_;
};

// История индикатора: файл с колонками date,value, отсортированный по дате.
std::vector<RateFixing> read_fixings(const std::string& path);

std::string format_date(Date date);

// Разбирает ISO-дату. false, если строка не является датой.
bool parse_date(const std::string& text, Date& date);

}  // namespace cfcore
