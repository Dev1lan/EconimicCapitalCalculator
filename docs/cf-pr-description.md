# Черновик Pull Request для develop/core

**Заголовок PR:** Cash Flow Engine — интеграция нормализатора, договорная политика V2 и финальная проверка

**Ветка:** `release/cf-core-final-validation` → `develop/core`. Проверенная база GitHub — `6fa3b5c343d271f7ee6f245256fcc501ab97cafb`.

## Описание

Ветка объединяет принятые четыре итерации. Банковский CSV и ручной JSON проходят через Python-нормализатор и расчёт в C++20. CLI работает с кириллическими путями на Windows и Ubuntu. Фиксации и даты SIMPLE валидируются. Repricing и трассировка выпускаются отдельными файлами.

По умолчанию используется `BASE`. `LEGACY_EXCEL` сохраняет 334 строки эталонной сделки. `CONTRACTUAL_V2` явно включает рабочий расчёт первого купона, отложенный PMT для ANN/FLOAT, итоговые договорные ставки и исторический fixing с календарным лагом. Контракт v1 сохранён, v2 добавляет два отдельных поля. V2 требует подтверждения куратора как банковская методика.

Финальная итерация добавляет 11 независимых числовых эталонов, совместимость v1/v2, контроль сделки №46, проверку всех 55 сделок и синтетический профиль для V2. Финансовый код этой итерацией не меняется.

## Проверка

1. Собрать Release и запустить `ctest` на Windows и Ubuntu. Отдельно проверить `Golden.DealOneMatchesSheetRowByRow`.
2. Установить пакет `cf-normalizer[dev]`, передать `CFCORE_BIN` и выполнить `python -m pytest cf-normalizer/tests -q`. Набор включает равенство ручной и портфельной сделки, проверку контрактов и независимые эталоны Decimal.
3. Выполнить `python tools/validate_release.py --cfcore <исполняемый файл> --output-dir out/final-mai`. Сверить 55 строк по ID и отчёты трёх политик. Для регрессии на той же ОС передать `--reference-dir` с эталонными результатами итерации 4.
4. По инструкции запуска проверить ручной JSON V2 с `events.csv` и `deal_trace.csv`. Технические события имеют нулевую денежную сумму.
5. При проверке производительности использовать синтетику и отдельно указывать наличие полного CSV-экспорта. [Команды и результаты](cf-release-validation.md) · [Сборка и запуск](cf-integration.md)

Финальный результат: Windows 102 CTest и 120 Python (1 пропущен), Ubuntu 103 CTest и 121 Python. На МАИ финансовые результаты совпали с итерацией 4, Excel сохранил 334 строки. [Построчный отчёт МАИ](validation/mai-deal-status.csv)

## Интерфейсы и совместимость

| Интерфейс | Изменение и совместимость |
|---|---|
| C++ CLI | Windows Unicode, экранирование JSON, `--policy base`, `--policy legacy-excel`, `--policy contractual-v2`, `--events`, `--trace-deal`. Существующие флаги и выбор BASE по умолчанию сохраняются. |
| Python | Пакет `normalizer` читает банковский CSV, поддерживает ручной JSON через `--manual` и передаёт политику и параметры диагностики в C++. |
| CSV и Parquet | v1 сохраняет 29 полей и прежнюю трактовку графика. v2 добавляет `repricing_final_rate_schedule` и `indicator_lag_days`. Parquet — дополнительный экспорт Python, ядро читает CSV. |
| C++ API | Расширены `Policy`, `Deal`, `DealArena`, `RunOptions` и структуры трассировки. `generate_cashflows` принимает необязательный указатель на трассировку. Бинарным потребителям нужно пересобраться. |
| Результаты | `cashflows.csv`, `deal_summary.csv`, `buckets.csv`, `errors.csv` и `events.csv` сохраняют форматы. Манифест добавляет `calculation_policy`. Трассировка включает источники ставки, PMT и начисления. |

Известные ограничения — RUB, карантин субсидий, сделка №20 с завышенным графиком и двусмысленный график сделки №46 в V2. Определения AA, лаг в рабочих днях, порядок капитализации и правила сценарной оценки требуют отдельного согласования. Расчёт БВДПК, опционов и Economic Capital в область PR не входит.

## Новые файлы относительно develop/core

- Корень и инструменты — `.gitattributes`, `tools/validate_release.py`.
- Python-пакет — `cf-normalizer/.gitignore`, `README.md`, `pyproject.toml`, `src/normalizer/{__init__,__main__,manual,processor,reader,validator,writer}.py`.
- Контракты — `cf-normalizer/contracts/normalized-deals-v1.json`, `normalized-deals-v2.json`, `cf-normalizer/docs/INP-14-data-contract-v1.md`, `INP-4-golden-tests.md`.
- Данные и тесты — `cf-normalizer/tests/data/тестовые_сделки_МАИ.csv`, `tests/golden/{core_deal_summary,core_errors,normalized_deals,quarantine}.csv`, `tests/test_{cfcore_integration,cli,contractual_v2,golden,processor,reader,release_reference,writer}.py`, `core/tests/test_cli.py`.
- Примеры — `examples/manual/{ann-fixed,ann-float,ann-float-v2,rgm-fixed,simple-fixed,simple-irregular}.json`.
- Передача — `docs/cf-integration.md`, `docs/cf-methodology-decisions.md`, `docs/cf-release-validation.md`, `docs/cf-pr-description.md`, `docs/validation/mai-deal-status.csv`.

Все принятые коммиты от `62724e1` до `c7477fc` входят в историю. Проверенный `develop/core` является предком ветки, конфликтов с этой базой нет. Перед публикацией снова сверить удалённый SHA. В этой итерации PR подготовлен локально.
