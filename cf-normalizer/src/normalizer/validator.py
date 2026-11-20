"""Проверки и преобразования полей контракта INP-14 v1.

Ошибки одной сделки не должны останавливать обработку остального файла.
"""

from datetime import date, datetime
from decimal import Decimal, InvalidOperation
import math
import re


_ISO_DATE = re.compile(r"\d{4}-\d{2}-\d{2}\Z")
_RU_DATE = re.compile(r"\d{2}\.\d{2}\.\d{4}\Z")
_INTEGER = re.compile(r"[+-]?\d+\Z")
_PERIOD = re.compile(r"([1-9]\d*)([MD])\Z")
_MIN_DATE = date(1900, 1, 1)
_MAX_DATE = date(2200, 1, 1)


class RowError(ValueError):
    """Причина попадания одной сделки в карантин."""

    def __init__(self, code: str):
        self.code = code
        super().__init__(code)


def parse_int(value: str, code: str, minimum: int, maximum: int) -> int:
    if not _INTEGER.fullmatch(value) or len(value.lstrip("+-")) > 19:
        raise RowError(code)
    number = int(value)
    if not minimum <= number <= maximum:
        raise RowError(code)
    return number


def parse_date(value: str, *, optional: bool = False) -> str | None:
    if not value:
        if optional:
            return None
        raise RowError("INVALID_DATE")
    try:
        if _ISO_DATE.fullmatch(value):
            parsed = date.fromisoformat(value)
        elif _RU_DATE.fullmatch(value):
            parsed = datetime.strptime(value, "%d.%m.%Y").date()
        else:
            raise ValueError("unexpected date format")
    except ValueError as exc:
        raise RowError("INVALID_DATE") from exc
    if not _MIN_DATE <= parsed <= _MAX_DATE:
        raise RowError("INVALID_DATE")
    return parsed.isoformat()


def parse_number(value: str, *, optional: bool = False) -> Decimal | None:
    if not value:
        if optional:
            return None
        raise RowError("INVALID_NUMBER")
    try:
        number = Decimal(value)
        as_float = float(number)
    except (InvalidOperation, ValueError, OverflowError) as exc:
        raise RowError("INVALID_NUMBER") from exc
    if not number.is_finite() or not math.isfinite(as_float):
        raise RowError("INVALID_NUMBER")
    if number != 0 and as_float == 0:
        raise RowError("INVALID_NUMBER")
    return number


def as_text(value: Decimal | None) -> str | None:
    return None if value is None else format(value, "f")


def parse_period(value: str) -> str | None:
    if not value:
        return None
    if value == "ENDDATE":
        return value
    match = _PERIOD.fullmatch(value)
    if not match or len(match.group(1)) > 10 or int(match.group(1)) > 2**31 - 1:
        raise RowError("INVALID_RULE")
    return value


def parse_day(value: str, *, repricing: bool = False) -> str | None:
    if not value:
        return None
    day = parse_int(value, "INVALID_RULE", -1 if repricing else 0, 31)
    return str(day)


def parse_dates(value: str) -> str | None:
    if not value:
        return None
    try:
        dates = [parse_date(item) for item in value.split()]
    except RowError as exc:
        raise RowError("INVALID_SCHEDULE") from exc
    return " ".join(sorted(dates))


def parse_pairs(value: str, *, nonnegative: bool) -> str | None:
    if not value:
        return None
    result: list[tuple[str, str]] = []
    try:
        for item in value.split():
            if item.count(",") != 1:
                raise RowError("INVALID_SCHEDULE")
            raw_date, raw_number = item.split(",")
            parsed_date = parse_date(raw_date)
            parsed_number = parse_number(raw_number)
            if nonnegative and parsed_number < 0:
                raise RowError("INVALID_SCHEDULE")
            result.append((parsed_date, as_text(parsed_number)))
    except RowError as exc:
        raise RowError("INVALID_SCHEDULE") from exc
    result.sort(key=lambda pair: pair[0])
    return " ".join(f"{d},{v}" for d, v in result)
