"""Временная замена нормализатора: тестовые сделки МАИ -> нормализованный CSV ядра.

Только для демонстрации, пока настоящий нормализатор не готов. Запуск:
python tools/normalize_stub.py data/input/test_deals_mai.csv out/deals.csv out/quarantine.csv
"""
import csv
import datetime as dt
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from normalized_format import COLUMNS  # noqa: E402

DAY_COUNTS = {"ACT/365": "ACT_365", "ACT/ACT": "ACT_ACT_ISDA", "ACT/ACT ISDA": "ACT_ACT_ISDA"}
GARBAGE_PRODUCTS = {"", "product"}


def iso(text):
    """дд.мм.гггг или уже ISO -> ISO; пусто остаётся пустым."""
    text = text.strip()
    if not text:
        return ""
    if "." in text:
        return dt.datetime.strptime(text, "%d.%m.%Y").date().isoformat()
    return text[:10]


def clean_list(text):
    return " ".join(sorted(iso(x) for x in text.split()))


def clean_dated_pairs(text):
    pairs = []
    for item in text.split():
        date, value = item.split(",")
        pairs.append((iso(date), value))
    pairs.sort()
    return " ".join(f"{d},{v}" for d, v in pairs)


def portfolio_id(row, keys):
    product = row["product"].strip()
    product = "" if product in GARBAGE_PRODUCTS else product
    key = (product, row["behavior_model_cd"].strip())
    return keys.setdefault(key, len(keys))


def quarantine_reason(row):
    if row["currency_id"] != "RUB":
        return "UNSUPPORTED_CURRENCY"
    if row["sub_rate_type_cd"].strip():
        return "SUB_BLOCK_UNSUPPORTED"
    return None


def normalize(row, keys):
    amount = float(row["current_principal_amt"])
    out = {name: row.get(name, "").strip() for name in COLUMNS}
    # ДОПУЩЕНИЕ: отрицательный остаток означает обязательство банка.
    out["side"] = "LIABILITY" if amount < 0 else "ASSET"
    out["current_principal_amt"] = repr(abs(amount))
    out["portfolio_key"] = portfolio_id(row, keys)
    out["day_convention_cd"] = DAY_COUNTS[row["day_convention_cd"].strip()]
    for name in ("report_dttm", "deal_dttm", "maturity_dttm", "interest_payment_rule_start_dttm",
                 "regular_payment_rule_start_dttm", "repricing_rule_start_dttm"):
        out[name] = iso(row[name])
    out["interest_payment"] = clean_list(row["interest_payment"])
    out["repricing_dttm_schedule"] = clean_list(row["repricing_dttm_schedule"])
    out["principal_payment"] = clean_dated_pairs(row["principal_payment"])
    out["repricing_dttm_rate_schedule_fixed"] = clean_dated_pairs(row["repricing_dttm_rate_schedule_fixed"])
    if row["rate_type_cd"] == "FLOAT":
        out["base_indicator_rate"] = ""  # значение берётся из истории КС через --fixings
    return out


def main():
    source, deals_path, quarantine_path = sys.argv[1:4]
    pathlib.Path(deals_path).parent.mkdir(parents=True, exist_ok=True)
    keys = {}
    with open(source, encoding="utf-8-sig", newline="") as f, \
            open(deals_path, "w", newline="") as deals_file, \
            open(quarantine_path, "w", newline="") as quarantine_file:
        deals = csv.DictWriter(deals_file, COLUMNS, delimiter=";", lineterminator="\n")
        deals.writeheader()
        quarantine = csv.writer(quarantine_file, lineterminator="\n")
        quarantine.writerow(["deal_id", "reason_code"])
        for row in csv.DictReader(f, delimiter=";"):
            reason = quarantine_reason(row)
            if reason:
                quarantine.writerow([row["deal_id"], reason])
            else:
                deals.writerow(normalize(row, keys))


if __name__ == "__main__":
    main()
