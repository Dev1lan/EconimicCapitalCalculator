"""Синтетический портфель в нормализованном формате ядра. Один seed даёт один и тот же файл.

Запуск: python tools/gen_synthetic.py out/synthetic.csv --deals 100000 --seed 42 --bad-share 0.001
Только стандартная библиотека Python.
"""
import argparse
import csv
import datetime as dt
import pathlib
import random
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from normalized_format import COLUMNS  # noqa: E402

REPORT = dt.date(2022, 8, 31)
INDICATOR = 0.08
PORTFOLIOS = 6


def random_deal(rng, deal_id):
    start = REPORT - dt.timedelta(days=rng.randrange(30, 3650))
    maturity = REPORT + dt.timedelta(days=rng.randrange(60, 10950))
    balance = round(rng.uniform(50_000, 15_000_000), 2)
    kind = rng.choices(["ANN", "RGM", "SIMPLE"], weights=[5, 3, 2])[0]
    is_float = rng.random() < 0.3
    day = rng.randrange(1, 29)
    deal = dict.fromkeys(COLUMNS, "")
    deal.update({
        "deal_id": deal_id,
        "report_dttm": REPORT.isoformat(),
        "discriminator": kind,
        "currency_id": "RUB",
        "side": "LIABILITY" if rng.random() < 0.15 else "ASSET",
        "portfolio_key": rng.randrange(PORTFOLIOS),
        "current_principal_amt": balance,
        "deal_dttm": start.isoformat(),
        "maturity_dttm": maturity.isoformat(),
        "rate_type_cd": "FLOAT" if is_float else "FIXED",
        "interest_rate": round(rng.uniform(0.005, 0.05) if is_float else rng.uniform(0.03, 0.2), 6),
        "day_convention_cd": rng.choice(["ACT_365", "ACT_ACT_ISDA"]),
        "interest_payment_rule_start_dttm": start.isoformat(),
        "interest_payment_rule_period_cd": "1M",
        "interest_payment_rule_day_num": day,
    })
    if is_float:
        deal["base_indicator_rate"] = INDICATOR
        deal["repricing_rule_start_dttm"] = start.isoformat()
        deal["repricing_rule_period_cd"] = rng.choice(["1M", "3M"])
        if rng.random() < 0.2:
            deal["cap_limit_interest_rate"] = 0.15
            deal["floor_limit_interest_rate"] = 0.02
    if kind == "SIMPLE":
        deal["principal_payment"] = simple_schedule(rng, balance, maturity)
    else:
        deal["regular_payment_rule_start_dttm"] = start.isoformat()
        deal["regular_payment_rule_period_cd"] = "1M"
        deal["regular_payment_rule_day_num"] = day
    return deal


def simple_schedule(rng, balance, maturity):
    """Несколько погашений с суммой, точно равной остатку."""
    parts = rng.randrange(1, 7)
    days = sorted(rng.sample(range(1, (maturity - REPORT).days), min(parts - 1, (maturity - REPORT).days - 1)))
    dates = [REPORT + dt.timedelta(days=d) for d in days] + [maturity]
    left = balance
    items = []
    for date in dates[:-1]:
        amount = round(left * rng.uniform(0.1, 0.4), 2)
        left -= amount
        items.append(f"{date.isoformat()},{amount}")
    items.append(f"{dates[-1].isoformat()},{repr(round(left, 2))}")
    return " ".join(items)


def spoil(rng, deal):
    """Намеренная ошибка в данных: ядро должно изолировать сделку."""
    kind = rng.randrange(3)
    if kind == 0:
        deal["current_principal_amt"] = "nan"
    elif kind == 1:
        deal["maturity_dttm"] = "2020-01-01"
    else:
        deal["discriminator"] = "UNKNOWN"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output")
    parser.add_argument("--deals", type=int, default=10000)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--bad-share", type=float, default=0.0)
    args = parser.parse_args()

    rng = random.Random(args.seed)
    pathlib.Path(args.output).parent.mkdir(parents=True, exist_ok=True)
    with open(args.output, "w", newline="") as f:
        writer = csv.DictWriter(f, COLUMNS, delimiter=";", lineterminator="\n")
        writer.writeheader()
        for deal_id in range(1, args.deals + 1):
            deal = random_deal(rng, deal_id)
            if rng.random() < args.bad_share:
                spoil(rng, deal)
            writer.writerow(deal)


if __name__ == "__main__":
    main()
