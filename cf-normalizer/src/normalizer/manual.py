"""Перевод одной ручной сделки в форму входной строки нормализатора."""

from decimal import Decimal
import json
from pathlib import Path

import polars as pl

from normalizer.validator import parse_number


_FIELDS = {
    "instrument_id": "deal_id",
    "reporting_date": "report_dttm",
    "start_date": "deal_dttm",
    "maturity_date": "maturity_dttm",
    "currency": "currency_id",
    "amortization": "discriminator",
    "rate_type": "rate_type_cd",
    "day_count": "day_convention_cd",
    "base_indicator_rate": "base_indicator_rate",
    "cap": "cap_limit_interest_rate",
    "floor": "floor_limit_interest_rate",
    "accrued_interest": "accrued_interest",
}
_RULES = {
    "interest_rule": "interest_payment_rule",
    "principal_rule": "regular_payment_rule",
    "repricing_rule": "repricing_rule",
}
_SCHEDULES = {
    "interest_payment_dates": ("interest_payment", None),
    "principal_payments": ("principal_payment", "amount"),
    "repricing_dates": ("repricing_dttm_schedule", None),
    "repricing_rates": ("repricing_dttm_rate_schedule_fixed", "rate"),
}
_FIELDS_WITH_SPECIAL_HANDLING = {"side", "outstanding_principal", "interest_rate", "spread"}


def _text(value, name: str) -> str | None:
    if value is None:
        return None
    if type(value) not in (str, int, Decimal):
        raise ValueError(f"{name}: ожидается строка или число")
    return str(value).strip()


def _rule(value, name: str, prefix: str) -> dict[str, str | None]:
    if value is None:
        return {}
    if not isinstance(value, dict) or set(value) - {"period", "start_date", "day"}:
        raise ValueError(f"{name}: ожидаются period, start_date и day")
    period = _text(value.get("period"), f"{name}.period")
    if not period:
        raise ValueError(f"{name}: требуется period")
    return {
        f"{prefix}_period_cd": period,
        f"{prefix}_start_dttm": _text(value.get("start_date"), f"{name}.start_date"),
        f"{prefix}_day_num": _text(value.get("day"), f"{name}.day"),
    }


def _schedule(value, name: str, value_key: str | None) -> str | None:
    if value is None or value == []:
        return None
    if not isinstance(value, list):
        raise ValueError(f"{name}: ожидается список")
    items = []
    for item in value:
        if value_key is None and isinstance(item, str) and item.strip():
            items.append(item.strip())
        elif value_key is not None and isinstance(item, dict) and set(item) == {"date", value_key}:
            date = _text(item["date"], f"{name}.date")
            amount = _text(item[value_key], f"{name}.{value_key}")
            if not date or not amount:
                raise ValueError(f"{name}: требуются date и {value_key}")
            items.append(f"{date},{amount}")
        else:
            raise ValueError(f"{name}: неверный элемент графика")
    return " ".join(items)


def read_manual_deal(path: Path) -> pl.DataFrame:
    data = json.loads(path.read_text(encoding="utf-8-sig"), parse_float=Decimal)
    if not isinstance(data, dict):
        raise ValueError("Ручная сделка должна быть одним объектом JSON")
    unknown = data.keys() - (_FIELDS.keys() | _RULES.keys() | _SCHEDULES.keys() | _FIELDS_WITH_SPECIAL_HANDLING)
    if unknown:
        raise ValueError(f"Неизвестные поля ручной сделки: {', '.join(sorted(unknown))}")

    row = {column: _text(data.get(name), name) for name, column in _FIELDS.items()}
    side = data.get("side", "ASSET")
    if side not in ("ASSET", "LIABILITY"):
        raise ValueError("side: ожидается ASSET или LIABILITY")
    principal = parse_number(_text(data.get("outstanding_principal"), "outstanding_principal") or "")
    if principal <= 0:
        raise ValueError("outstanding_principal: ожидается положительный остаток")
    row["current_principal_amt"] = format(
        principal.copy_negate() if side == "LIABILITY" else principal, "f"
    )

    rate = _text(data.get("interest_rate"), "interest_rate")
    spread = _text(data.get("spread"), "spread")
    if spread is not None and (row["rate_type_cd"] != "FLOAT" or rate is not None):
        raise ValueError("spread допустим только для FLOAT без interest_rate")
    row["interest_rate"] = spread if spread is not None else rate

    for name, prefix in _RULES.items():
        row.update(_rule(data.get(name), name, prefix))
    for name, (column, value_key) in _SCHEDULES.items():
        row[column] = _schedule(data.get(name), name, value_key)

    amortization = row["discriminator"]
    if row["principal_payment"] and amortization != "SIMPLE":
        raise ValueError("principal_payments поддерживается ядром только для SIMPLE")
    if amortization == "SIMPLE" and row.get("regular_payment_rule_period_cd") not in (None, "ENDDATE"):
        raise ValueError("SIMPLE: используйте principal_payments или ENDDATE")
    if row["interest_payment"] and (amortization == "ANN" or data.get("interest_rule") is not None):
        raise ValueError("interest_payment_dates несовместим с ANN или с interest_rule")
    if amortization == "ANN" and data.get("interest_rule") is not None and data.get("principal_rule") is not None:
        raise ValueError("ANN: задайте одно правило общего платёжного графика")

    return pl.DataFrame([row], schema={column: pl.String for column in row})
