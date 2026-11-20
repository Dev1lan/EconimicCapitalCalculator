"""Нормализация банковских сделок по контракту INP-14.

Модуль не рассчитывает денежные потоки и не записывает файлы: это задачи
ядра C++ и writer.py соответственно.
"""

from collections import Counter
from dataclasses import dataclass
import polars as pl

from normalizer.validator import (
    RowError,
    as_text,
    parse_date,
    parse_dates,
    parse_day,
    parse_int,
    parse_number,
    parse_pairs,
    parse_period,
)


OUTPUT_COLUMNS = (
    "deal_id", "report_dttm", "discriminator", "currency_id", "side", "portfolio_key",
    "current_principal_amt", "deal_dttm", "maturity_dttm", "rate_type_cd", "interest_rate",
    "base_indicator_rate", "cap_limit_interest_rate", "floor_limit_interest_rate",
    "accrued_interest", "day_convention_cd",
    "interest_payment_rule_start_dttm", "interest_payment_rule_period_cd", "interest_payment_rule_day_num",
    "regular_payment_rule_start_dttm", "regular_payment_rule_period_cd", "regular_payment_rule_day_num",
    "repricing_rule_start_dttm", "repricing_rule_period_cd", "repricing_rule_day_num",
    "interest_payment", "principal_payment", "repricing_dttm_schedule",
    "repricing_dttm_rate_schedule_fixed",
)

V2_FIELDS = ("repricing_final_rate_schedule", "indicator_lag_days")
OUTPUT_COLUMNS_V2 = OUTPUT_COLUMNS + V2_FIELDS

REQUIRED_SOURCE_COLUMNS = (
    "deal_id", "report_dttm", "discriminator", "currency_id", "current_principal_amt",
    "deal_dttm", "maturity_dttm", "rate_type_cd", "interest_rate", "day_convention_cd",
)

_DAY_COUNTS = {
    "ACT/365": "ACT_365",
    "ACT/ACT": "ACT_ACT_ISDA",
    "ACT/ACT ISDA": "ACT_ACT_ISDA",
}


@dataclass(frozen=True)
class NormalizationResult:
    deals: pl.DataFrame
    quarantine: pl.DataFrame
    portfolio_keys: dict[tuple[str, str], int]


def _cell(row: dict[str, str | None], name: str) -> str:
    value = row.get(name)
    return "" if value is None else str(value).strip()


def _duplicate_ids(deals: pl.DataFrame) -> set[int]:
    counts: Counter[int] = Counter()
    for raw in deals.get_column("deal_id"):
        try:
            number = parse_int(
                "" if raw is None else str(raw).strip(), "INVALID_ID", -(2**63), 2**63 - 1
            )
        except RowError:
            continue
        counts[number] += 1
    return {number for number, count in counts.items() if count > 1}


def _normalize_row(
    row: dict[str, str | None],
    duplicate_ids: set[int],
    portfolio_keys: dict[tuple[str, str], int],
    use_external_fixings: bool,
    contractual_v2: bool,
) -> dict[str, str | None]:
    # Порядок проверок соответствует приоритету причин карантина в INP-14.
    if _cell(row, "currency_id") != "RUB":
        raise RowError("UNSUPPORTED_CURRENCY")
    if any(name.startswith("sub_") and _cell(row, name) for name in row):
        raise RowError("SUB_BLOCK_UNSUPPORTED")
    if any(not _cell(row, name) for name in REQUIRED_SOURCE_COLUMNS):
        raise RowError("MISSING_REQUIRED")

    deal_id = parse_int(_cell(row, "deal_id"), "INVALID_ID", -(2**63), 2**63 - 1)
    if deal_id in duplicate_ids:
        raise RowError("INVALID_ID")

    amortization = _cell(row, "discriminator")
    rate_type = _cell(row, "rate_type_cd")
    day_convention = _DAY_COUNTS.get(_cell(row, "day_convention_cd"))
    if amortization not in {"ANN", "RGM", "SIMPLE"} or rate_type not in {"FIXED", "FLOAT"}:
        raise RowError("INVALID_ENUM")
    if day_convention is None:
        raise RowError("INVALID_ENUM")

    # Даты и числовые поля проверяем до правил и явных списков.
    report = parse_date(_cell(row, "report_dttm"))
    start = parse_date(_cell(row, "deal_dttm"))
    maturity = parse_date(_cell(row, "maturity_dttm"))
    if maturity <= report:
        raise RowError("INVALID_DATE")

    result: dict[str, str | None] = {name: None for name in OUTPUT_COLUMNS}
    rule_prefixes = (
        "interest_payment_rule", "regular_payment_rule", "repricing_rule",
    )
    for prefix in rule_prefixes:
        name = f"{prefix}_start_dttm"
        result[name] = parse_date(_cell(row, name), optional=True)

    principal = parse_number(_cell(row, "current_principal_amt"))
    if principal == 0:
        raise RowError("INVALID_NUMBER")
    interest_rate = parse_number(_cell(row, "interest_rate"))
    indicator_rate = parse_number(_cell(row, "base_indicator_rate"), optional=True)
    cap = parse_number(_cell(row, "cap_limit_interest_rate"), optional=True)
    floor = parse_number(_cell(row, "floor_limit_interest_rate"), optional=True)
    accrued_interest = parse_number(_cell(row, "accrued_interest"), optional=True)
    if rate_type == "FIXED" and interest_rate < 0:
        raise RowError("INVALID_NUMBER")
    if cap is not None and floor is not None and floor > cap:
        raise RowError("INVALID_NUMBER")

    for prefix in rule_prefixes:
        result[f"{prefix}_period_cd"] = parse_period(_cell(row, f"{prefix}_period_cd"))
        result[f"{prefix}_day_num"] = parse_day(
            _cell(row, f"{prefix}_day_num"), repricing=prefix == "repricing_rule"
        )

    result["interest_payment"] = parse_dates(_cell(row, "interest_payment"))
    result["principal_payment"] = parse_pairs(
        _cell(row, "principal_payment"), nonnegative=True
    )
    result["repricing_dttm_schedule"] = parse_dates(_cell(row, "repricing_dttm_schedule"))
    result["repricing_dttm_rate_schedule_fixed"] = parse_pairs(
        _cell(row, "repricing_dttm_rate_schedule_fixed"), nonnegative=False
    )

    if contractual_v2:
        if _cell(row, "indicator_lag_cd"):
            raise RowError("UNSUPPORTED_INDICATOR_LAG")
        if _cell(row, "repricing_dttm_rate_schedule") or (
            rate_type == "FLOAT" and result["repricing_dttm_rate_schedule_fixed"]
        ):
            raise RowError("AMBIGUOUS_RATE_SCHEDULE")
        result["repricing_final_rate_schedule"] = parse_pairs(
            _cell(row, "repricing_final_rate_schedule"), nonnegative=False
        )
        lag = _cell(row, "indicator_lag_days")
        result["indicator_lag_days"] = (
            str(parse_int(lag, "INVALID_INDICATOR_LAG", 0, 2**31 - 1)) if lag else None
        )
    elif any(_cell(row, name) for name in V2_FIELDS):
        raise RowError("UNSUPPORTED")

    # Ключ создаём только после успешной валидации строки.
    product = _cell(row, "product")
    if product == "product":
        product = ""
    key = (product, _cell(row, "behavior_model_cd"))
    if key not in portfolio_keys:
        portfolio_keys[key] = len(portfolio_keys)

    result.update({
        "deal_id": str(deal_id),
        "report_dttm": report,
        "discriminator": amortization,
        "currency_id": "RUB",
        "side": "LIABILITY" if principal < 0 else "ASSET",
        "portfolio_key": str(portfolio_keys[key]),
        "current_principal_amt": as_text(abs(principal)),
        "deal_dttm": start,
        "maturity_dttm": maturity,
        "rate_type_cd": rate_type,
        "interest_rate": as_text(interest_rate),
        "base_indicator_rate": (
            None if rate_type == "FLOAT" and use_external_fixings else as_text(indicator_rate)
        ),
        "cap_limit_interest_rate": as_text(cap),
        "floor_limit_interest_rate": as_text(floor),
        "accrued_interest": as_text(accrued_interest),
        "day_convention_cd": day_convention,
    })
    return result


def normalize_deals(
    deals: pl.DataFrame, *, use_external_fixings: bool = True, contractual_v2: bool = False
) -> NormalizationResult:
    """Нормализовать таблицу reader.read_csv в память, не вычисляя CF.

    При use_external_fixings=True для FLOAT очищается base_indicator_rate,
    как в демонстрационном ядре с параметром --fixings. При False исходное
    значение индикатора сохраняется. contractual_v2 явно выбирает схему v2.

    Returns:
        deals: строковые колонки в порядке контракта, null — отсутствие.
        quarantine: исходный deal_id и код причины отклонения.
        portfolio_keys: группировка, нужная для интерпретации ключей.

    Raises:
        ValueError: нет обязательных колонок исходного файла.
    """
    missing = [name for name in REQUIRED_SOURCE_COLUMNS if name not in deals.columns]
    if missing:
        raise ValueError(f"Отсутствуют обязательные колонки CSV: {', '.join(missing)}")
    if len(set(deals.columns)) != len(deals.columns):
        raise ValueError("В CSV повторяются имена колонок")

    duplicate_ids = _duplicate_ids(deals)
    accepted: list[dict[str, str | None]] = []
    rejected: list[dict[str, str | None]] = []
    portfolio_keys: dict[tuple[str, str], int] = {}

    for row in deals.iter_rows(named=True):
        try:
            accepted.append(_normalize_row(row, duplicate_ids, portfolio_keys, use_external_fixings, contractual_v2))
        except RowError as exc:
            rejected.append({"deal_id": _cell(row, "deal_id") or None, "reason_code": exc.code})

    columns = OUTPUT_COLUMNS_V2 if contractual_v2 else OUTPUT_COLUMNS
    text_schema = {name: pl.String for name in columns}
    normalized = pl.DataFrame(accepted, schema=text_schema)
    quarantine = pl.DataFrame(rejected, schema={"deal_id": pl.String, "reason_code": pl.String})
    return NormalizationResult(normalized, quarantine, portfolio_keys)
