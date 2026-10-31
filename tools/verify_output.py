"""Независимая проверка результата ядра: инварианты считаются заново из выходных файлов.

Запуск: python tools/verify_output.py out/run
"""
import collections
import csv
import pathlib
import sys

TOLERANCE = 1e-4  # по одной сделке
TOTAL_TOLERANCE_RUB = 0.01  # по всему портфелю
UNITS_PER_RUB = 10**10  # суммы в файлах записаны с 10 знаками


def rows(path):
    with open(path, newline="") as f:
        yield from csv.DictReader(f)


def units(text):
    """Сумма с 10 знаками как целое число: обычное сложение float на десятках
    миллионов строк накапливает ошибку больше рубля и даёт ложное расхождение."""
    return int(text.replace(".", ""))


def main():
    out = pathlib.Path(sys.argv[1])
    problems = []

    deals = {row["instrument_id"]: row for row in rows(out / "deal_summary.csv")}
    for deal_id, row in deals.items():
        if abs(float(row["closing_balance"])) > TOLERANCE:
            problems.append(f"сделка {deal_id}: остаток {row['closing_balance']}")
        if abs(float(row["total_principal"]) - float(row["opening_principal"])) > TOLERANCE:
            problems.append(f"сделка {deal_id}: долг погашен не полностью")

    flow_sum = {"PRINCIPAL": 0, "INTEREST": 0}
    per_deal = collections.Counter()
    flows = 0
    for row in rows(out / "cashflows.csv"):
        amount = float(row["amount"])
        if amount < 0 or amount != amount:
            problems.append(f"сделка {row['instrument_id']}: неверная сумма {row['amount']}")
            continue
        flow_sum[row["cash_flow_type"]] += units(row["amount"])
        per_deal[row["instrument_id"]] += 1
        flows += 1

    bucket_principal = bucket_interest = 0
    for row in rows(out / "buckets.csv"):
        bucket_principal += units(row["principal_cf"])
        bucket_interest += units(row["interest_cf"])
    if abs(bucket_principal - flow_sum["PRINCIPAL"]) / UNITS_PER_RUB > TOTAL_TOLERANCE_RUB:
        problems.append("сумма долга по бакетам не равна сумме потоков")
    if abs(bucket_interest - flow_sum["INTEREST"]) / UNITS_PER_RUB > TOTAL_TOLERANCE_RUB:
        problems.append("сумма процентов по бакетам не равна сумме потоков")

    for deal_id, row in deals.items():
        if per_deal[deal_id] != int(row["flow_count"]):
            problems.append(f"сделка {deal_id}: число потоков не совпадает")

    print(f"сделок: {len(deals)}, потоков: {flows}")
    print(f"основной долг: потоки {flow_sum['PRINCIPAL'] / UNITS_PER_RUB:.2f}, "
          f"бакеты {bucket_principal / UNITS_PER_RUB:.2f}")
    print(f"проценты:      потоки {flow_sum['INTEREST'] / UNITS_PER_RUB:.2f}, "
          f"бакеты {bucket_interest / UNITS_PER_RUB:.2f}")
    if problems:
        print(f"НАЙДЕНО ПРОБЛЕМ: {len(problems)}")
        for problem in problems[:20]:
            print(" ", problem)
        sys.exit(1)
    print("все проверки пройдены")


if __name__ == "__main__":
    main()
