"""CLI: банковский CSV или ручная сделка -> нормализованные файлы -> cfcore."""

import argparse
import json
from pathlib import Path
import subprocess
import sys

import polars as pl

from normalizer.manual import read_manual_deal_with_policy
from normalizer.processor import normalize_deals
from normalizer.reader import read_csv
from normalizer.writer import write_csv, write_parquet, write_quarantine


POLICIES = ("base", "legacy-excel", "contractual-v2")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Нормализация входных сделок CF")
    parser.add_argument("input", type=Path, help="банковский CSV или JSON при --manual")
    parser.add_argument("--manual", action="store_true", help="рассчитать одну сделку из JSON через cfcore")
    parser.add_argument("--output-dir", type=Path, required=True, help="папка результатов")
    parser.add_argument("--format", choices=("csv", "parquet", "both"), default="both")
    parser.add_argument("--cfcore", type=Path, help="путь к собранному C++-исполняемому файлу cfcore")
    parser.add_argument("--fixings", type=Path, help="история индикатора в формате date,value")
    parser.add_argument("--threads", type=int, help="число потоков для cfcore")
    parser.add_argument("--events", action="store_true", help="вывести events.csv с пересмотрами ставки")
    parser.add_argument("--trace-deal", type=int, help="вывести deal_trace.csv по instrument_id")
    parser.add_argument("--no-flows", action="store_true", help="не записывать cashflows.csv")
    parser.add_argument("--excel-etalon", action="store_true", help="использовать правила ядра для Excel-эталона")
    parser.add_argument("--policy", choices=POLICIES, help="расчётная политика, по умолчанию base")
    parser.add_argument("--risk-buckets", choices=("8mr",), help="регуляторные бакеты отдельно от договорных")
    parser.add_argument("--risk-coupons", type=Path, help="CSV с определённостью FLOAT-купонов")
    parser.add_argument("--prepayment-model", choices=("none", "bvdpk"))
    parser.add_argument("--prepayment-eligible", type=Path)
    parser.add_argument("--prepayment-scenario")
    parser.add_argument("--bvdpk")
    parser.add_argument("--bvdpk-file", type=Path)
    parser.add_argument("--bvdpk-source", choices=("demonstration", "bank"))
    args = parser.parse_args(argv)
    prepayment_options = (
        ("--prepayment-model", args.prepayment_model),
        ("--prepayment-eligible", args.prepayment_eligible),
        ("--prepayment-scenario", args.prepayment_scenario),
        ("--bvdpk", args.bvdpk),
        ("--bvdpk-file", args.bvdpk_file),
        ("--bvdpk-source", args.bvdpk_source),
    )

    if args.excel_etalon and args.policy not in (None, "legacy-excel"):
        parser.error("--excel-etalon несовместим с выбранной политикой")

    if args.manual and args.cfcore is None:
        parser.error("--manual требует --cfcore")
    if args.cfcore is None and (args.events or args.trace_deal is not None or args.no_flows or args.excel_etalon
                               or args.risk_buckets or args.risk_coupons
                               or any(value is not None for _, value in prepayment_options)):
        parser.error("параметры вывода и режима ядра требуют --cfcore")

    if args.cfcore and args.format == "parquet":
        parser.error("cfcore пока читает только CSV; используйте --format csv или both")
    if args.threads is not None and args.threads <= 0:
        parser.error("--threads должно быть положительным числом")
    if args.fixings is not None and not args.fixings.is_file():
        parser.error(f"файл истории ставки не найден: {args.fixings}")
    if args.cfcore is not None and not args.cfcore.is_file():
        parser.error(f"исполняемый файл cfcore не найден: {args.cfcore}")

    try:
        source, manual_policy = read_manual_deal_with_policy(args.input) if args.manual else (read_csv(args.input), None)
        cli_policy = "legacy-excel" if args.excel_etalon else args.policy
        if manual_policy is not None and manual_policy not in POLICIES:
            raise ValueError(f"Неизвестная расчётная политика: {manual_policy}")
        if cli_policy is not None and manual_policy is not None and cli_policy != manual_policy:
            raise ValueError("Политики в JSON и CLI должны совпадать")
        policy = cli_policy or manual_policy
        result = normalize_deals(source, use_external_fixings=args.fixings is not None,
                                 contractual_v2=policy == "contractual-v2")
        output_dir = args.output_dir
        output_dir.mkdir(parents=True, exist_ok=True)

        if args.format in {"csv", "both"}:
            path = write_csv(result.deals, output_dir / "normalized_deals.csv")
            print(f"CSV: {path}")
        if args.format in {"parquet", "both"}:
            path = write_parquet(result.deals, output_dir / "normalized_deals.parquet")
            print(f"Parquet: {path}")

        quarantine_path = write_quarantine(result.quarantine, output_dir / "quarantine.csv")
        print(f"Карантин: {quarantine_path}")
        print(f"Обработано: {source.height}; принято: {result.deals.height}; отклонено: {result.quarantine.height}")

        trace_deal = args.trace_deal
        if args.manual:
            if result.deals.is_empty():
                reason = result.quarantine["reason_code"][0]
                print(f"Ручная сделка отклонена: {reason}. См. {quarantine_path}", file=sys.stderr)
                return 2
            instrument_id = int(result.deals["deal_id"][0])
            if trace_deal is not None and trace_deal != instrument_id:
                raise ValueError("--trace-deal должен совпадать с instrument_id ручной сделки")
            trace_deal = instrument_id

        if (args.risk_buckets or args.prepayment_model == "bvdpk") and result.quarantine.height:
            raise ValueError("Для расчёта риск-бакетов или БВДПК вход должен быть без карантина")
        if args.cfcore is not None:
            core_dir = output_dir / "core"
            command = [
                str(args.cfcore.resolve()), "--input", str((output_dir / "normalized_deals.csv").resolve()),
                "--output-dir", str(core_dir.resolve()),
            ]
            if args.fixings is not None:
                command.extend(["--fixings", str(args.fixings.resolve())])
            if args.threads is not None:
                command.extend(["--threads", str(args.threads)])
            if args.events or args.manual:
                command.append("--events")
            if trace_deal is not None:
                command.extend(["--trace-deal", str(trace_deal)])
            if args.no_flows:
                command.append("--no-flows")
            if args.risk_buckets:
                command.extend(["--risk-buckets", args.risk_buckets])
            if args.risk_coupons:
                command.extend(["--risk-coupons", str(args.risk_coupons.resolve())])
            for name, value in prepayment_options:
                if value is not None:
                    command.extend([name, str(value.resolve() if isinstance(value, Path) else value)])
            if policy is not None:
                command.extend(["--policy", policy])
            print(f"Запуск cfcore; результаты: {core_dir}", flush=True)
            completed = subprocess.run(command, check=False)
            if completed.returncode or not args.manual:
                return completed.returncode
            manifest = json.loads((core_dir / "run_manifest.json").read_text(encoding="utf-8"))
            if manifest["deals_ok"] != 1:
                print(f"Ручная сделка не рассчитана. См. {core_dir / 'errors.csv'}", file=sys.stderr)
                return 2
    except (OSError, ValueError, subprocess.SubprocessError, pl.exceptions.PolarsError) as exc:
        parser.exit(2, f"Ошибка: {exc}\n")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
