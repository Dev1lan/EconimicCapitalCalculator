"""Расчёт нормализованного файла сделок: ./cf file путь.csv

На выходе cashflows.csv и deal_summary.csv в out/cli/file_<имя файла>/.
С флагом --summary-only только deal_summary.csv. Считает ядро cfcore, при необходимости оно собирается.
"""
import argparse
import pathlib
import shutil
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
BINARY = ROOT / "build" / "core" / "cfcore"
OUT = ROOT / "out" / "cli"
FIXINGS = ROOT / "data" / "input" / "key_rate.csv"


def ensure_built():
    sources = [p for p in (ROOT / "core").rglob("*") if p.suffix in {".cpp", ".hpp", ".txt"}]
    newest = max(p.stat().st_mtime for p in sources)
    if BINARY.exists() and BINARY.stat().st_mtime >= newest:
        return
    subprocess.run(["cmake", "-S", str(ROOT), "-B", str(ROOT / "build"), "-DCMAKE_BUILD_TYPE=Release"],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["cmake", "--build", str(ROOT / "build"), "-j8", "--target", "cfcore_cli"],
                   check=True, stdout=subprocess.DEVNULL)


def command_file(args):
    path = pathlib.Path(args.path).expanduser().resolve()
    if not path.is_file():
        print(f"Файл не найден: {path}", file=sys.stderr)
        return 1
    ensure_built()
    out_dir = OUT / f"file_{path.stem}"
    shutil.rmtree(out_dir, ignore_errors=True)
    command = [str(BINARY), "--input", str(path), "--output-dir", str(out_dir), "--fixings", str(FIXINGS),
               "--details-only"]
    if args.summary_only:
        command.append("--no-flows")
    started = time.monotonic()
    result = subprocess.run(command)
    elapsed = time.monotonic() - started
    if result.returncode == 0:
        if not args.summary_only:
            print(out_dir / "cashflows.csv")
        print(out_dir / "deal_summary.csv")
        print(f"Время: {elapsed:.3f} с")
    return result.returncode


def main():
    parser = argparse.ArgumentParser(prog="./cf", description="Расчёт денежных потоков ядром cfcore.")
    commands = parser.add_subparsers(dest="command", required=True)
    file = commands.add_parser("file", help="посчитать нормализованный файл")
    file.add_argument("path")
    file.add_argument("--summary-only", action="store_true", help="писать только deal_summary.csv")
    file.set_defaults(handler=command_file)
    args = parser.parse_args()
    try:
        return args.handler(args)
    except KeyboardInterrupt:
        print()
        return 130
    except subprocess.CalledProcessError as error:
        print(f"Ошибка сборки ядра: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
