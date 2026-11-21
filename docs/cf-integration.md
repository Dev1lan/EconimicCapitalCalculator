# Запуск Cash Flow из банка или ручной сделки

Python CLI использует reader, processor и writer из `cf-normalizer`. Ручной JSON преобразуется в одну входную строку для того же processor. В обоих случаях C++ получает CSV по контракту v1 и выполняет расчёт. Схема 29 колонок и единицы измерения остались прежними.

## Windows PowerShell

Команды из корня репозитория. Для многоконфигурационной сборки executable находится в `build/core/Release`, для Ninja — в `build/core`.

```powershell
python -m pip install -e './cf-normalizer[dev]'
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
ctest --test-dir build -C Release --output-on-failure

$cfcore = (Get-Item ./build/core/Release/cfcore.exe,./build/core/cfcore.exe -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
$env:CFCORE_BIN = $cfcore
python -m pytest cf-normalizer/tests -q

python -m normalizer ./data/input/test_deals_mai.csv --output-dir ./out/normalized --format csv --fixings ./data/input/key_rate.csv
python -m normalizer ./data/input/test_deals_mai.csv --output-dir ./out/portfolio --format csv --cfcore $cfcore --fixings ./data/input/key_rate.csv
python -m normalizer ./data/input/test_deals_mai.csv --output-dir './out/Отчёты МАИ' --format csv --cfcore $cfcore --fixings ./data/input/key_rate.csv --events --trace-deal 44
python -m normalizer ./examples/manual/ann-float.json --manual --output-dir ./out/manual --format csv --cfcore $cfcore
```

## POSIX

```bash
python3 -m pip install -e './cf-normalizer[dev]'
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 8
ctest --test-dir build --output-on-failure

cfcore="$(pwd)/build/core/cfcore"
CFCORE_BIN="$cfcore" python3 -m pytest cf-normalizer/tests -q

python3 -m normalizer data/input/test_deals_mai.csv --output-dir out/normalized --format csv --fixings data/input/key_rate.csv
python3 -m normalizer data/input/test_deals_mai.csv --output-dir out/portfolio --format csv --cfcore "$cfcore" --fixings data/input/key_rate.csv
python3 -m normalizer data/input/test_deals_mai.csv --output-dir 'out/Отчёты МАИ' --format csv --cfcore "$cfcore" --fixings data/input/key_rate.csv --events --trace-deal 44
python3 -m normalizer examples/manual/ann-float.json --manual --output-dir out/manual --format csv --cfcore "$cfcore"
```

В Linux с системным ограничением на установку Python-пакетов запустите команды в виртуальном окружении. После установки пакет можно вызывать из другого каталога, передав пути, относительные к текущему каталогу, или абсолютные. Сборка ищет GoogleTest и при его отсутствии скачивает зависимость. Встроенные проверки CLI через CTest запускаются при наличии Python 3. POSIX-сценарий `bash tools/demo.sh 100` дополнительно прогоняет синтетику.

## Результаты и связь файлов

- `normalized_deals.csv` и `quarantine.csv` создаются в указанном каталоге. `--format both` также создаёт Parquet. Если указан `--fixings`, нормализатор сохраняет существующую политику v1 и очищает `base_indicator_rate` у FLOAT, даже в режиме одной нормализации. При последующем запуске ядра передайте тот же файл истории.
- В подкаталоге `core` находятся `cashflows.csv`, `deal_summary.csv`, `buckets.csv`, `errors.csv`, `run_manifest.json`. Флаги `--events`, `--trace-deal ID` и `--no-flows` можно комбинировать. Ручной режим включает оба отчёта, а `cash_amount` в трассировке показывает сумму выплат на строке.
- В нормализованном файле `deal_id` соответствует `instrument_id` у потоков, сводки, ошибок ядра, пересмотров и трассировки. Карантин хранит исходный `deal_id`, который может отсутствовать или быть некорректным. Бакеты агрегированы и связываются через `portfolio_key,currency,side` нормализованной строки (`currency_id` соответствует выходному `currency`).
- Тест МАИ по умолчанию берёт `data/input/key_rate.csv`. SHA-256 считается после замены CRLF на LF. Содержимое и порядок строк контролируются исходным хешем, поэтому ручная конвертация не нужна.

## Поля на границе нормализатора и ядра

| Исходный CSV | Нормализованный CSV v1 | C++ и назначение |
|---|---|---|
| `deal_id` | `deal_id`, целое int64 | `instrument_id` во всех строках уровня сделки |
| `report_dttm`, `deal_dttm`, `maturity_dttm` | Те же поля, даты ISO | `report_date`, `start_date`, `maturity_date` для отбора событий и начала начисления |
| `current_principal_amt`, `currency_id` | Абсолютный остаток, `side` по знаку, только RUB | `outstanding_principal`, `side`, `currency` для расчёта и группировки |
| `discriminator`, `rate_type_cd` | `ANN/RGM/SIMPLE`, `FIXED/FLOAT` | `amortization`, `rate_type` для расчёта платежей и ставки |
| `interest_rate`, `base_indicator_rate`, cap/floor | Десятичные доли, необязательные поля могут быть пустыми | `rate`, `indicator_rate`, `cap`, `floor`. В FLOAT по действующей политике `rate` — спред. При внешней истории индикатор сделки очищается. |
| `interest_payment_rule_*`, `regular_payment_rule_*`, `repricing_rule_*` | ISO, периоды вида `1M`, `30D`, `ENDDATE`, номер дня | `interest_rule`, `principal_rule`, `reset_rule` для построения графика |
| `interest_payment`, `principal_payment` | Списки дат и пар `дата,сумма` через пробел | `interest_schedule`, `principal_schedule`. ANN использует общий график. Явный график тела используется в SIMPLE. |
| `repricing_dttm_schedule`, `repricing_dttm_rate_schedule_fixed` | Списки дат и пар `дата,ставка` через пробел | `reset_schedule`. Явное значение меняет компонент ставки по текущей политике. |
| `accrued_interest` | Пусто, 0 или заданная сумма | `std::optional<double>` в `Deal`. Сводка различает `REPORTED` и `COMPUTED`, график сохраняет правила текущего ядра. |
| `day_convention_cd` | `ACT_365` или `ACT_ACT_ISDA` | `day_count` для доли года |
| `product`, `behavior_model_cd` | `portfolio_key` из пары значений | Ключ агрегации в пределах запуска |

Контракт v1 опускает `base_indicator` (код индекса), `indicator_lag_cd` и отдельное поле `repricing_dttm_rate_schedule`. В исходном CSV МАИ последнее поле пустое, а используемое `repricing_dttm_rate_schedule_fixed` — другая колонка. Для v2 предлагаются явный идентификатор индекса, единица лага и семантика значения в графике. Валюты вне RUB и заполненные `sub_*` отправляются в карантин. [Схема CSV v1](../cf-normalizer/contracts/normalized-deals-v1.json) · [Решения по методике](cf-methodology-decisions.md)

## Ручной JSON

[Пять примеров](../examples/manual) покрывают SIMPLE/FIXED, RGM/FIXED, ANN/FIXED, ANN/FLOAT и SIMPLE с нерегулярным телом.

- Обязательны `instrument_id`, `reporting_date`, `start_date`, `maturity_date`, положительный `outstanding_principal`, `currency`, `amortization`, `rate_type` и `day_count`. Для FIXED задайте `interest_rate`. Для FLOAT задайте `spread` или `interest_rate` как спред. Числа ставок — доли. Например, 12% записывается как `0.12`. Значения day count: `ACT/365`, `ACT/ACT`, `ACT/ACT ISDA`.
- `side` по умолчанию `ASSET`. Для `LIABILITY` адаптер передаёт нормализатору отрицательный знак остатка. Необязательные `base_indicator_rate`, `accrued_interest`, `cap`, `floor` можно опустить или задать `null`. Для FLOAT нужен индикатор сделки или история через `--fixings`. Явный `0` в `accrued_interest` сохраняется.
- `interest_rule`, `principal_rule` и `repricing_rule` принимают объект с `period` и необязательными `start_date` и `day`. Если дату начала правила опустить, C++ использует дату начала сделки. Пустые или отсутствующие списки остаются пустыми на входе ядра. Без явного графика тела SIMPLE гасит остаток в дату окончания по действующей политике.
- `interest_payment_dates` и `repricing_dates` — списки дат. `principal_payments` — объекты `{date, amount}` для SIMPLE. `repricing_rates` — объекты `{date, rate}` с прежней интерпретацией ядра. Для ANN используйте одно месячное правило общего графика. Адаптер отклоняет графики, которые выбранная амортизация иначе проигнорировала бы.
- Ручной режим возвращает код 2 при отклонении сделки. Причина нормализации записывается в карантин, а причина отказа ядра — в `core/errors.csv`.

Эталонный режим включается флагом `--excel-etalon`. Рабочие правила для будущего исправления первого купона, исторического fixing и ANN + FLOAT описаны отдельно. [Методология](cf-methodology-decisions.md)

Для расчётной политики CONTRACTUAL_V2 укажите `--policy contractual-v2` в командной строке
или `"policy": "contractual-v2"` в JSON сделки. Используйте итоговые ставки из [контракта v2](../cf-normalizer/contracts/normalized-deals-v2.json).
[Пример ANN/FLOAT в V2](../examples/manual/ann-float-v2.json)

CONTRACTUAL_V2 содержит рабочие решения команды и требует согласования с куратором перед использованием в качестве банковской методики. Отчёт по всем сделкам МАИ, независимые эталоны, условия нагрузки и инструкция проверки PR описаны отдельно. [Финальная проверка](cf-release-validation.md)
