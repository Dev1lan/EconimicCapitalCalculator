"""Простой интерфейс к ядру cfcore: меню и короткие команды для защиты.

Запуск из корня репозитория: ./cf (меню) или ./cf --help (команды).
Нужен только Python 3 и собранное ядро; при необходимости сборка запускается сама.
Расчёт всегда делает cfcore. Здесь данные выбираются, показываются и сверяются.
"""
import argparse
import calendar
import collections
import csv
import datetime as dt
import pathlib
import shutil
import subprocess
import sys

from normalized_format import COLUMNS

ROOT = pathlib.Path(__file__).resolve().parent.parent
BINARY = ROOT / "build" / "core" / "cfcore"
TOOLS = ROOT / "tools"
OUT = ROOT / "out" / "cli"
MAI_SOURCE = ROOT / "data" / "input" / "test_deals_mai.csv"
MAI_DEALS = ROOT / "out" / "mai" / "deals.csv"
MAI_QUARANTINE = ROOT / "out" / "mai" / "quarantine.csv"
FIXINGS = ROOT / "data" / "input" / "key_rate.csv"
GOLDEN = ROOT / "data" / "golden"

CUSTOM_DEAL_ID = 900001
SHORT_TABLE_LIMIT = 14
MONEY_TOLERANCE = 0.005

BUCKET_LABELS = [
    "до востребования", "до 1 мес.", "1-3 мес.", "3-6 мес.", "6-9 мес.", "9-12 мес.",
    "1-1,5 года", "1,5-2 года", "2-3 года", "3-4 года", "4-5 лет", "5-6 лет", "6-7 лет",
    "7-8 лет", "8-9 лет", "9-10 лет", "10-15 лет", "15-20 лет", "свыше 20 лет",
]
BUCKET_UPPER_MONTHS = [1, 3, 6, 9, 12, 18, 24, 36, 48, 60, 72, 84, 96, 108, 120, 180, 240]

ERROR_TEXT = {
    "UNSUPPORTED": "тип сделки или валюта не поддерживаются",
    "INVALID_NUMBER": "неверное число: остаток, ставка, cap или floor",
    "INVALID_DATE": "неверная дата: срок должен быть позже даты отчёта",
    "INVALID_SCHEDULE": "неверный график платежей",
    "SCHEDULE_SUM_MISMATCH": "сумма будущего графика больше остатка долга",
    "SCHEDULE_ENDS_EARLY": "сумма будущего графика меньше остатка долга",
    "MISSING_RATE": "нет значения ключевой ставки для плавающей сделки",
    "TOO_MANY_EVENTS": "слишком много платежей",
    "NON_FINITE_RESULT": "в расчёте появилось нечисловое значение",
    "PARSE_ERROR": "строка входного файла не разобрана",
}
QUARANTINE_TEXT = {
    "UNSUPPORTED_CURRENCY": "валюта не RUB, в первую версию не входит",
    "SUB_BLOCK_UNSUPPORTED": "заполнен блок sub_*, в первую версию не входит",
}


def money(value):
    return f"{value:,.2f}".replace(",", " ").replace(".", ",")


def percent(value):
    return f"{value * 100:.4f}".rstrip("0").rstrip(".").replace(".", ",") + "%"


def title(text):
    print(f"\n=== {text} ===")


def table(headers, rows, aligns):
    """aligns: строка из l и r на каждую колонку."""
    cells = [[str(c) for c in row] for row in rows]
    widths = [max(len(h), *(len(r[i]) for r in cells)) if cells else len(h) for i, h in enumerate(headers)]

    def line(values):
        parts = [v.ljust(w) if a == "l" else v.rjust(w) for v, w, a in zip(values, widths, aligns)]
        return "  ".join(parts).rstrip()

    print(line(headers))
    print("  ".join("-" * w for w in widths))
    for row in cells:
        print(line(row))


def ensure_built():
    sources = [p for p in (ROOT / "core").rglob("*") if p.suffix in {".cpp", ".hpp", ".txt"}]
    newest = max(p.stat().st_mtime for p in sources)
    if BINARY.exists() and BINARY.stat().st_mtime >= newest:
        return
    print("Собираю ядро...")
    subprocess.run(["cmake", "-S", str(ROOT), "-B", str(ROOT / "build"), "-DCMAKE_BUILD_TYPE=Release"],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["cmake", "--build", str(ROOT / "build"), "-j8", "--target", "cfcore_cli"],
                   check=True, stdout=subprocess.DEVNULL)


def run_core(deals_path, out_dir, excel=False, flows=True):
    ensure_built()
    shutil.rmtree(out_dir, ignore_errors=True)
    command = [str(BINARY), "--input", str(deals_path), "--output-dir", str(out_dir), "--fixings", str(FIXINGS)]
    if excel:
        command.append("--excel-etalon")
    if not flows:
        command.append("--no-flows")
    return subprocess.run(command, check=True, capture_output=True, text=True).stdout.strip()


def ensure_mai():
    if not MAI_DEALS.exists():
        subprocess.run([sys.executable, str(TOOLS / "normalize_stub.py"), str(MAI_SOURCE), str(MAI_DEALS),
                        str(MAI_QUARANTINE)], check=True)


def read_mai():
    ensure_mai()
    with open(MAI_DEALS, newline="") as f:
        deals = {row["deal_id"]: row for row in csv.DictReader(f, delimiter=";")}
    with open(MAI_QUARANTINE, newline="") as f:
        quarantine = {row["deal_id"]: row["reason_code"] for row in csv.DictReader(f)}
    return deals, quarantine


def write_deal(deal, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(f, COLUMNS, delimiter=";", lineterminator="\n")
        writer.writeheader()
        writer.writerow(deal)


def read_csv(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def add_months(date, months):
    total = date.year * 12 + (date.month - 1) + months
    year, month = total // 12, total % 12 + 1
    return date.replace(year=year, month=month, day=min(date.day, calendar.monthrange(year, month)[1]))


def bucket_of(date, report):
    """Отдельный от ядра расчёт бакета: результат сверяется с buckets.csv."""
    if date <= report:
        return 0
    for k, months in enumerate(BUCKET_UPPER_MONTHS, 1):
        if date <= add_months(report, months):
            return k
    return 18


def payment_rows(flows, opening, report):
    """Объединяет проценты и долг одной даты в строку графика."""
    by_date = {}
    for flow in flows:
        date = dt.date.fromisoformat(flow["payment_date"])
        row = by_date.setdefault(date, {"date": date, "interest": 0.0, "principal": 0.0, "rate": None, "fraction": 0.0})
        amount = float(flow["amount"])
        if flow["cash_flow_type"] == "INTEREST":
            row.update(interest=amount, rate=float(flow["rate_used"]), fraction=float(flow["year_fraction"]))
        else:
            row["principal"] = amount
    rows = []
    balance = opening
    for date in sorted(by_date):
        row = by_date[date]
        row["before"] = balance
        expected = None if row["rate"] is None else balance * row["rate"] * row["fraction"]
        row["check"] = "" if expected is None else ("да" if abs(expected - row["interest"]) < MONEY_TOLERANCE else "составная")
        balance -= row["principal"]
        row["after"] = balance
        row["bucket"] = bucket_of(date, report)
        rows.append(row)
    return rows


def print_conditions(deal):
    title(f"Сделка {deal['deal_id']}: условия")
    print(f"Тип графика:     {deal['discriminator']}")
    print(f"Сторона:         {deal['side']}")
    print(f"Остаток долга:   {money(float(deal['current_principal_amt']))}")
    rate = float(deal["interest_rate"])
    if deal["rate_type_cd"] == "FLOAT":
        print(f"Ставка:          плавающая, индикатор + {percent(rate)}")
    else:
        print(f"Ставка:          фиксированная, {percent(rate)}")
    for name, label in (("cap_limit_interest_rate", "Cap"), ("floor_limit_interest_rate", "Floor")):
        if deal[name]:
            print(f"{label + ':':<16} {percent(float(deal[name]))}")
    print(f"Дата отчёта:     {deal['report_dttm']}")
    print(f"Начало и срок:   {deal['deal_dttm']} - {deal['maturity_dttm']}")
    print(f"Счёт дней:       {deal['day_convention_cd']}")
    print(f"Период платежей: долг {deal['regular_payment_rule_period_cd'] or 'не задан'}, "
          f"проценты {deal['interest_payment_rule_period_cd'] or 'не задан'}")
    if deal["principal_payment"]:
        print(f"График долга:    {deal['principal_payment']}")
    if deal["interest_payment"]:
        print(f"Даты процентов:  {deal['interest_payment']}")


def print_schedule(rows, show_all):
    title("График платежей (посчитан ядром)")
    shown = rows
    if len(rows) > SHORT_TABLE_LIMIT and not show_all:
        shown = rows[:8] + [None] + rows[-4:]
    data = []
    for number, row in enumerate(rows, 1):
        row["number"] = number
    for row in shown:
        if row is None:
            data.append(["...", "...", "", "", "", "", "", "", "", "", ""])
            continue
        rate = "" if row["rate"] is None else percent(row["rate"])
        fraction = "" if row["rate"] is None else f"{row['fraction']:.6f}".replace(".", ",")
        data.append([row["number"], row["date"].isoformat(), money(row["before"]), rate, fraction,
                     money(row["interest"]), money(row["principal"]), money(row["interest"] + row["principal"]),
                     money(row["after"]), row["bucket"], row["check"]])
    table(["N", "Дата", "Долг до", "Ставка", "Доля года", "Проценты", "Погашение", "Платёж", "Долг после",
           "Бакет", "Проверка %"], data, "rlrrrrrrrrl")
    if shown is not rows:
        print(f"Показано не всё, строк {len(rows)}. Весь график: добавьте --all")
    print("Проверка %: «да» — проценты равны долгу × ставка × доля года.")
    print("«составная» — внутри периода менялся долг или ставка, начисление идёт по участкам.")


def print_formula(deal, rows):
    title("Формула первого платежа")
    first = next((r for r in rows if r["rate"] is not None), None)
    if first is None:
        return
    fraction = f"{first['fraction']:.6f}".replace(".", ",")
    print(f"Проценты = долг × ставка × доля года = {money(first['before'])} × {percent(first['rate'])} × "
          f"{fraction} = {money(first['interest'])}")
    balance = float(deal["current_principal_amt"])
    count = sum(1 for r in rows if r["principal"] > 0)
    if deal["discriminator"] == "ANN" and first["rate"] > 0 and count > 1:
        i = first["rate"] / 12
        payment = balance * i / (1 - (1 + i) ** (-count))
        print(f"Аннуитет = B × i / (1 − (1 + i)^(−N)): B = {money(balance)}, i = {percent(first['rate'])} / 12, "
              f"N = {count}, платёж = {money(payment)}")
        print(f"Погашение = платёж − проценты = {money(payment)} − {money(first['interest'])} = "
              f"{money(payment - first['interest'])}")
    elif deal["discriminator"] == "RGM":
        print(f"Равные части: B / N = {money(balance)} / {count} = {money(balance / count)}")


def print_buckets(rows, out_dir):
    title("Раскладка по бакетам (buckets.csv)")
    data = []
    from_rows = collections.defaultdict(float)
    for row in rows:
        from_rows[row["bucket"]] += row["principal"] + row["interest"]
    matched = True
    for row in read_csv(out_dir / "buckets.csv"):
        k = int(row["bucket"])
        principal, interest = float(row["principal_cf"]), float(row["interest_cf"])
        matched = matched and abs(from_rows[k] - principal - interest) < MONEY_TOLERANCE
        data.append([k, BUCKET_LABELS[k], money(principal), money(interest), money(principal + interest)])
    table(["k", "Интервал", "Долг", "Проценты", "Всего"], data, "rlrrr")
    print("Сверка с построчным графиком и отдельным расчётом бакетов: " + ("совпало" if matched else "РАСХОЖДЕНИЕ"))


def print_totals(deal, out_dir, excel):
    summary = read_csv(out_dir / "deal_summary.csv")[0]
    title("Итог по сделке")
    opening = float(summary["opening_principal"])
    principal = float(summary["total_principal"])
    closing = float(summary["closing_balance"])
    print(f"Погашено долга:     {money(principal)} из {money(opening)}")
    print(f"Проценты всего:     {money(float(summary['total_interest']))}")
    print(f"Денежный поток:     {money(principal + float(summary['total_interest']))}")
    print(f"Остаток в конце:    {money(closing)}" + ("  (в Excel остаток не гасится)" if excel and closing > 0 else ""))
    source = summary["current_interest_source"]
    value = summary["current_interest"]
    print(f"Current interest:   {money(float(value)) if value else 'нет данных'}  ({source})")
    print(f"Потоков в файле:    {summary['flow_count']}")
    print(f"Файлы результата:   {out_dir.relative_to(ROOT)}/")


def show_deal(deal, out_dir, excel=False, show_all=False):
    path = out_dir.parent / f"{out_dir.name}_input.csv"
    write_deal(deal, path)
    run_core(path, out_dir, excel=excel)
    print_conditions(deal)
    print(f"Режим:           {'правила листа Excel' if excel else 'базовый'}")
    errors = read_csv(out_dir / "errors.csv")
    if errors:
        code = errors[0]["error_code"]
        title("Сделка не рассчитана")
        print(f"{code}: {ERROR_TEXT.get(code, code)}")
        return False
    rows = payment_rows(read_csv(out_dir / "cashflows.csv"), float(deal["current_principal_amt"]),
                        dt.date.fromisoformat(deal["report_dttm"]))
    print_schedule(rows, show_all)
    print_formula(deal, rows)
    print_buckets(rows, out_dir)
    print_totals(deal, out_dir, excel)
    return True


def command_deal(args):
    deals, quarantine = read_mai()
    deal_id = str(args.deal_id)
    if deal_id in quarantine:
        reason = quarantine[deal_id]
        print(f"Сделка {deal_id} не считается: {QUARANTINE_TEXT.get(reason, reason)}.")
        return 0
    if deal_id not in deals:
        print(f"Сделки {deal_id} нет. Список: ./cf list")
        return 1
    return 0 if show_deal(deals[deal_id], OUT / f"deal_{deal_id}", args.excel, args.all) else 1


def command_list(_args):
    deals, quarantine = read_mai()
    data = []
    for deal in deals.values():
        data.append([deal["deal_id"], deal["discriminator"], deal["rate_type_cd"], deal["side"],
                     money(float(deal["current_principal_amt"])), percent(float(deal["interest_rate"])),
                     deal["maturity_dttm"]])
    title("Тестовые сделки МАИ")
    table(["N", "Тип", "Ставка", "Сторона", "Остаток", "Ставка/надбавка", "Срок"], data, "rlllrrl")
    print(f"\nНе считаются: {', '.join(quarantine)} (другая валюта и блок sub_*)")
    return 0


def command_etalon(_args):
    """Сверка сделки 1 со строками листа «1 сделка»."""
    deals, _ = read_mai()
    out_dir = OUT / "etalon"
    show_deal(deals["1"], out_dir, excel=True)

    title("Сверка с листом Excel «1 сделка»")
    golden = read_csv(GOLDEN / "deal1_flows.csv")
    report = dt.date.fromisoformat(deals["1"]["report_dttm"])
    rows = payment_rows(read_csv(out_dir / "cashflows.csv"), float(deals["1"]["current_principal_amt"]), report)
    if len(rows) != len(golden):
        print(f"РАСХОЖДЕНИЕ: строк {len(rows)}, в Excel {len(golden)}")
        return 1
    worst = collections.Counter()
    wrong_rows = 0
    for row, expected in zip(rows, golden):
        wrong_rows += row["date"].isoformat() != expected["date"] or row["bucket"] != int(expected["bucket"])
        worst["principal"] = max(worst["principal"], abs(row["principal"] - float(expected["principal"])))
        worst["interest"] = max(worst["interest"], abs(row["interest"] - float(expected["interest"])))
        worst["balance"] = max(worst["balance"], abs(row["after"] - float(expected["balance"])))
    print(f"Строк сверено:                  {len(rows)}")
    print(f"Несовпадений даты или бакета:   {wrong_rows}")
    print(f"Макс. разница по долгу:         {worst['principal']:.2e} руб.")
    print(f"Макс. разница по процентам:     {worst['interest']:.2e} руб.")
    print(f"Макс. разница по остатку:       {worst['balance']:.2e} руб.")

    vector = collections.defaultdict(float)
    for row in read_csv(out_dir / "buckets.csv"):
        vector[int(row["bucket"])] += float(row["principal_cf"]) + float(row["interest_cf"])
    factors = read_csv(GOLDEN / "deal1_df.csv")
    base = sum(vector[int(f["bucket"])] * float(f["df_base"]) for f in factors)
    stress = sum(vector[int(f["bucket"])] * float(f["df_stress"]) for f in factors)
    title("Контроль NPV по дисконт-факторам листа (только сверка, ядро DF не использует)")
    number = lambda value: f"{value:,.4f}".replace(",", " ").replace(".", ",")
    table(["", "Ядро", "Excel"], [
        ["NPV база", number(base), "2 056 999,4971"],
        ["NPV стресс", number(stress), "1 892 477,0845"],
        ["dNPV", number(stress - base), "-164 522,4126"],
    ], "lrr")
    return 0 if wrong_rows == 0 else 1


def ask(prompt, default=None, convert=str):
    label = f"{prompt} [{default}]: " if default is not None else f"{prompt}: "
    while True:
        text = input(label).strip()
        if not text and default is not None:
            text = str(default)
        try:
            return convert(text)
        except ValueError as error:
            print(f"  Неверное значение: {error}")


def parse_date(text):
    try:
        if "." in text:
            return dt.datetime.strptime(text, "%d.%m.%Y").date().isoformat()
        return dt.date.fromisoformat(text).isoformat()
    except ValueError:
        raise ValueError("нужна дата ГГГГ-ММ-ДД или ДД.ММ.ГГГГ") from None


def parse_choice(*options):
    def convert(text):
        value = text.upper()
        if value not in options:
            raise ValueError("нужно одно из: " + ", ".join(options))
        return value
    return convert


def parse_number(text):
    try:
        return float(text.replace(",", ".").replace(" ", ""))
    except ValueError:
        raise ValueError("нужно число") from None


def parse_optional_number(text):
    return "" if text == "-" else parse_number(text)


def parse_day(text):
    if not text.isdigit() or not 1 <= int(text) <= 31:
        raise ValueError("день от 1 до 31")
    return int(text)


def parse_principal_list(text):
    """Формат: 2022-12-31 250000; 2023-04-30 750000"""
    if text == "-":
        return ""
    items = []
    for item in text.split(";"):
        parts = item.split()
        if len(parts) != 2:
            raise ValueError("нужен формат: 2022-12-31 250000; 2023-04-30 750000")
        items.append((parse_date(parts[0]), parse_number(parts[1])))
    return " ".join(f"{d},{a}" for d, a in sorted(items))


def custom_deal():
    title("Ввод сделки. Enter оставляет значение в скобках, ставки в процентах")
    deal = dict.fromkeys(COLUMNS, "")
    kind = ask("Тип графика ANN, RGM или SIMPLE", "ANN", parse_choice("ANN", "RGM", "SIMPLE"))
    balance = ask("Остаток долга", 1000000, parse_number)
    rate_type = ask("Ставка FIXED или FLOAT", "FIXED", parse_choice("FIXED", "FLOAT"))
    rate_label = "Надбавка к ключевой ставке, %" if rate_type == "FLOAT" else "Ставка, %"
    rate = ask(rate_label, 2 if rate_type == "FLOAT" else 12, parse_number) / 100
    report = ask("Дата отчёта", "2022-08-31", parse_date)
    start = ask("Начало сделки", "2022-01-15", parse_date)
    maturity = ask("Дата погашения", "2023-01-15", parse_date)
    day = ask("День месяца для платежа", dt.date.fromisoformat(start).day, parse_day)
    period = ask("Период платежей, например 1M, 3M, 45D, ENDDATE", "1M", str.upper)
    convention = ask("Счёт дней ACT_365 или ACT_ACT_ISDA", "ACT_365", parse_choice("ACT_365", "ACT_ACT_ISDA"))
    deal.update({
        "deal_id": CUSTOM_DEAL_ID, "report_dttm": report, "discriminator": kind, "currency_id": "RUB",
        "side": ask("Сторона ASSET или LIABILITY", "ASSET", parse_choice("ASSET", "LIABILITY")),
        "portfolio_key": 0, "current_principal_amt": balance, "deal_dttm": start, "maturity_dttm": maturity,
        "rate_type_cd": rate_type, "interest_rate": rate, "day_convention_cd": convention,
        "interest_payment_rule_start_dttm": start, "interest_payment_rule_period_cd": period,
        "interest_payment_rule_day_num": day,
    })
    if kind == "SIMPLE":
        deal["principal_payment"] = ask("График долга: дата сумма; дата сумма. Минус: всё в дату погашения",
                                        "-", parse_principal_list)
    else:
        deal.update({"regular_payment_rule_start_dttm": start, "regular_payment_rule_period_cd": period,
                     "regular_payment_rule_day_num": day})
    if rate_type == "FLOAT":
        indicator = ask("Ключевая ставка, %. Минус: взять из истории на дату отчёта", "-", parse_optional_number)
        if indicator != "":
            deal["base_indicator_rate"] = indicator / 100
    cap = ask("Cap, %. Минус: нет", "-", parse_optional_number)
    floor = ask("Floor, %. Минус: нет", "-", parse_optional_number)
    deal["cap_limit_interest_rate"] = "" if cap == "" else cap / 100
    deal["floor_limit_interest_rate"] = "" if floor == "" else floor / 100
    return deal


def command_custom(args):
    return 0 if show_deal(custom_deal(), OUT / "custom", args.excel, args.all) else 1


def print_errors(path):
    errors = read_csv(path)
    counts = collections.Counter(row["error_code"] for row in errors)
    if counts:
        title("Сделки с ошибкой")
        for code, count in counts.most_common():
            print(f"{count:>8}  {code}: {ERROR_TEXT.get(code, code)}")


def print_portfolio_buckets(out_dir):
    title("Итог по бакетам: все портфели")
    principal, interest = collections.Counter(), collections.Counter()
    for row in read_csv(out_dir / "buckets.csv"):
        k = int(row["bucket"])
        sign = -1.0 if row["side"] == "LIABILITY" else 1.0
        principal[k] += sign * float(row["principal_cf"])
        interest[k] += sign * float(row["interest_cf"])
    data = [[k, BUCKET_LABELS[k], money(principal[k]), money(interest[k])] for k in sorted(principal)]
    table(["k", "Интервал", "Долг", "Проценты"], data, "rlrr")
    print("Обязательства показаны со знаком минус.")


def verify(out_dir):
    title("Проверка инвариантов")
    sys.stdout.flush()
    return subprocess.run([sys.executable, str(TOOLS / "verify_output.py"), str(out_dir)]).returncode


def command_mai(_args):
    ensure_mai()
    out_dir = OUT / "mai"
    print(run_core(MAI_DEALS, out_dir))
    print_errors(out_dir / "errors.csv")
    print_portfolio_buckets(out_dir)
    return verify(out_dir)


def file_format(path):
    """По заголовку отличает нормализованный файл от сырого файла банка."""
    with open(path, encoding="utf-8-sig", newline="") as f:
        columns = set(f.readline().strip().split(";"))
    if set(COLUMNS) <= columns:
        return "normalized"
    if "sub_rate_type_cd" in columns:
        return "raw"
    return None


def command_file(args):
    path = pathlib.Path(args.path).expanduser().resolve()
    if not path.is_file():
        print(f"Файл не найден: {path}")
        return 1
    kind = file_format(path)
    if kind is None:
        print("Формат не распознан. Нужен разделитель ';' и колонки сырого файла МАИ "
              "или нормализованного файла (раздел 4 README).")
        return 1
    out_dir = OUT / f"file_{path.stem}"
    deals_path = path
    quarantine = []
    if kind == "raw":
        print("Сырой файл: прогоняю через временный нормализатор.")
        deals_path = OUT / f"file_{path.stem}_deals.csv"
        quarantine_path = OUT / f"file_{path.stem}_quarantine.csv"
        deals_path.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([sys.executable, str(TOOLS / "normalize_stub.py"), str(path), str(deals_path),
                        str(quarantine_path)], check=True)
        quarantine = read_csv(quarantine_path)
    print(run_core(deals_path, out_dir, flows=not args.no_flows))
    if quarantine:
        title("Не считаются, отсеяны до ядра")
        for reason, count in collections.Counter(row["reason_code"] for row in quarantine).most_common():
            print(f"{count:>8}  {reason}: {QUARANTINE_TEXT.get(reason, reason)}")
    print_errors(out_dir / "errors.csv")
    print_portfolio_buckets(out_dir)
    if args.no_flows:
        print(f"\nФайлы результата: {out_dir}")
        return 0
    code = verify(out_dir)
    print(f"\nКэшфлоу: {out_dir / 'cashflows.csv'}")
    return code


def command_portfolio(args):
    deals_path = OUT / "synthetic_deals.csv"
    print(f"Генерирую {args.deals} сделок, seed {args.seed}, испорчено {args.bad_share:.1%}...")
    subprocess.run([sys.executable, str(TOOLS / "gen_synthetic.py"), str(deals_path), "--deals", str(args.deals),
                    "--seed", str(args.seed), "--bad-share", str(args.bad_share)], check=True)
    out_dir = OUT / "synthetic"
    print(run_core(deals_path, out_dir, flows=args.flows))
    print_errors(out_dir / "errors.csv")
    print_portfolio_buckets(out_dir)
    return verify(out_dir) if args.flows else 0


def command_test(_args):
    ensure_built()
    subprocess.run(["cmake", "--build", str(ROOT / "build"), "-j8"], check=True, stdout=subprocess.DEVNULL)
    result = subprocess.run(["ctest", "--test-dir", str(ROOT / "build")], capture_output=True, text=True)
    print(next(line for line in result.stdout.splitlines() if "tests passed" in line or "tests failed" in line))
    return result.returncode


def build_parser():
    parser = argparse.ArgumentParser(prog="./cf", description="Расчётное ядро cfcore. Без команды открывается меню.")
    commands = parser.add_subparsers(dest="command")

    deal = commands.add_parser("deal", help="разобрать сделку МАИ построчно")
    deal.add_argument("deal_id", type=int)
    deal.add_argument("--all", action="store_true", help="показать весь график")
    deal.add_argument("--excel", action="store_true", help="правила листа Excel")
    deal.set_defaults(handler=command_deal)

    commands.add_parser("list", help="список сделок МАИ").set_defaults(handler=command_list)
    commands.add_parser("etalon", help="сверка сделки 1 с Excel").set_defaults(handler=command_etalon)

    custom = commands.add_parser("custom", help="ввести свою сделку")
    custom.add_argument("--all", action="store_true", help="показать весь график")
    custom.add_argument("--excel", action="store_true", help="правила листа Excel")
    custom.set_defaults(handler=command_custom)

    commands.add_parser("mai", help="посчитать все сделки МАИ").set_defaults(handler=command_mai)

    file = commands.add_parser("file", help="посчитать свой CSV-файл")
    file.add_argument("path")
    file.add_argument("--no-flows", action="store_true", help="не писать cashflows.csv, для больших файлов")
    file.set_defaults(handler=command_file)

    portfolio = commands.add_parser("portfolio", help="синтетический портфель")
    portfolio.add_argument("deals", type=int, nargs="?", default=10000)
    portfolio.add_argument("--seed", type=int, default=42)
    portfolio.add_argument("--bad-share", type=float, default=0.001)
    portfolio.add_argument("--flows", action="store_true", help="писать cashflows.csv и проверить инварианты")
    portfolio.set_defaults(handler=command_portfolio)

    commands.add_parser("test", help="запустить тесты").set_defaults(handler=command_test)
    return parser


def menu():
    items = [
        ("Разобрать сделку МАИ по номеру", lambda: command_deal(argparse.Namespace(
            deal_id=ask("Номер сделки", 1, int), excel=False, all=False))),
        ("Показать список сделок МАИ", lambda: command_list(None)),
        ("Сверить сделку 1 с Excel", lambda: command_etalon(None)),
        ("Ввести свою сделку", lambda: command_custom(argparse.Namespace(excel=False, all=False))),
        ("Посчитать все сделки МАИ", lambda: command_mai(None)),
        ("Посчитать свой файл", lambda: command_file(argparse.Namespace(
            path=ask("Путь к файлу"), no_flows=False))),
        ("Синтетический портфель", lambda: command_portfolio(argparse.Namespace(
            deals=ask("Число сделок", 10000, int), seed=42, bad_share=0.001, flows=False))),
        ("Запустить тесты", lambda: command_test(None)),
    ]
    while True:
        title("Расчётное ядро cfcore")
        for number, (label, _) in enumerate(items, 1):
            print(f"{number}. {label}")
        print("0. Выход")
        choice = input("> ").strip()
        if choice == "0":
            return 0
        if choice.isdigit() and 1 <= int(choice) <= len(items):
            items[int(choice) - 1][1]()
        else:
            print("Нужен номер пункта.")


def main():
    args = build_parser().parse_args()
    try:
        return args.handler(args) if args.command else menu()
    except (EOFError, KeyboardInterrupt):
        print()
        return 0
    except subprocess.CalledProcessError as error:
        print(f"Ошибка запуска: {error.stderr or error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
