"""CLI: банковский CSV -> нормализованные файлы -> опционально cfcore."""

import argparse
from pathlib import Path
import subprocess

import polars as pl

from normalizer.processor import normalize_deals
from normalizer.reader import read_csv
from normalizer.writer import write_csv, write_parquet, write_quarantine


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Нормализация входных сделок CF")
    parser.add_argument("input", type=Path, help="исходный банковский CSV")
    parser.add_argument("--output-dir", type=Path, required=True, help="папка результатов")
    parser.add_argument("--format", choices=("csv", "parquet", "both"), default="both")
    parser.add_argument("--cfcore", type=Path, help="путь к собранному C++-исполняемому файлу cfcore")
    parser.add_argument("--fixings", type=Path, help="история индикатора в формате date,value")
    parser.add_argument("--threads", type=int, help="число потоков для cfcore")
    parser.add_argument("--events", action="store_true", help="вывести events.csv с пересмотрами ставки")
    parser.add_argument("--trace-deal", type=int, help="вывести deal_trace.csv по instrument_id")
    parser.add_argument("--no-flows", action="store_true", help="не записывать cashflows.csv")
    parser.add_argument("--excel-etalon", action="store_true", help="использовать правила ядра для Excel-эталона")
    args = parser.parse_args(argv)

    if args.cfcore is None and (args.events or args.trace_deal is not None or args.no_flows or args.excel_etalon):
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
        source = read_csv(args.input)
        result = normalize_deals(source, use_external_fixings=args.fixings is not None)
        output_dir = args.output_dir
        output_dir.mkdir(parents=True, exist_ok=True)

        if args.format in {"csv", "both"}:
            path = write_csv(result.deals, output_dir / "normalized_deals.csv")
            print(f"CSV: {path}")
        if args.format in {"parquet", "both"}:
            path = write_parquet(result.deals, output_dir / "normalized_deals.parquet")
            print(f"Parquet: {path}")

        path = write_quarantine(result.quarantine, output_dir / "quarantine.csv")
        print(f"Карантин: {path}")
        print(f"Обработано: {source.height}; принято: {result.deals.height}; отклонено: {result.quarantine.height}")

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
            if args.events:
                command.append("--events")
            if args.trace_deal is not None:
                command.extend(["--trace-deal", str(args.trace_deal)])
            if args.no_flows:
                command.append("--no-flows")
            if args.excel_etalon:
                command.append("--excel-etalon")
            print(f"Запуск cfcore; результаты: {core_dir}", flush=True)
            return subprocess.run(command, check=False).returncode
    except (OSError, ValueError, subprocess.SubprocessError, pl.exceptions.PolarsError) as exc:
        parser.exit(2, f"Ошибка: {exc}\n")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
