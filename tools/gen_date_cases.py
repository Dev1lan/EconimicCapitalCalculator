"""Эталонные даты для тестов ядра: считаются средствами Python, независимо от C++.

Запуск: python tools/gen_date_cases.py (нужен python-dateutil).
"""
import calendar
import csv
import datetime as dt
import pathlib
import random

from dateutil.relativedelta import relativedelta

OUT = pathlib.Path(__file__).resolve().parent.parent / "core" / "tests" / "data"
EPOCH = dt.date(1970, 1, 1)
FIRST = dt.date(1900, 1, 1)
LAST = dt.date(2199, 12, 31)
CASES = 5000
EDGE_DAYS = [
    dt.date(2020, 2, 29), dt.date(2024, 2, 29), dt.date(1900, 2, 28), dt.date(2000, 2, 29),
    dt.date(2021, 1, 31), dt.date(2022, 8, 31), dt.date(2022, 12, 31), dt.date(2023, 1, 1),
]


def random_day(rng):
    return FIRST + dt.timedelta(days=rng.randrange((LAST - FIRST).days + 1))


def write_dates(rng):
    days = EDGE_DAYS + [random_day(rng) for _ in range(CASES)]
    with open(OUT / "dates.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["days", "year", "month", "day"])
        for d in days:
            w.writerow([(d - EPOCH).days, d.year, d.month, d.day])


def write_add_months(rng):
    with open(OUT / "add_months.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["days", "months", "day", "result_days"])
        for i in range(CASES):
            d = EDGE_DAYS[i % len(EDGE_DAYS)] if i < 400 else random_day(rng)
            months = rng.randrange(-480, 481)
            shifted = d + relativedelta(months=months)
            if i % 2 == 0:
                # день исходной даты: сверка с relativedelta
                day, result = 0, shifted
            else:
                # заданный день: сверка с длиной месяца из calendar
                day = rng.randrange(1, 32)
                last = calendar.monthrange(shifted.year, shifted.month)[1]
                result = shifted.replace(day=min(day, last))
            w.writerow([(d - EPOCH).days, months, day, (result - EPOCH).days])


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    rng = random.Random(20220831)
    write_dates(rng)
    write_add_months(rng)


if __name__ == "__main__":
    main()
