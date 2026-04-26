#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import re
import sys
from pathlib import Path
from typing import Dict, List


COLUMNS = [
    "resolution",
    "ssp",
    "depth",
    "mpi",
    "openmp",
    "cores used",
    "time used in hours",
]


def parse_allocated_cpus(line: str) -> int | None:
    match = re.search(r"Allocated CPUs:\s*([0-9]+)", line)
    if match is None:
        return None
    return int(match.group(1))


def parse_latest_metrics_row(line: str) -> str | None:
    marker = "Latest Metrics Row:"
    if marker not in line:
        return None
    return line.split(marker, 1)[1].strip()


def collect_report_files(input_dir: Path) -> List[Path]:
    files: List[Path] = []
    for path in input_dir.rglob("*"):
        if not path.is_file():
            continue
        if path.suffix.lower() not in {".txt", ".log"}:
            continue
        files.append(path)
    files.sort()
    return files


def resolution_sort_key(value: object) -> tuple[int, int, str]:
    text = str(value)
    parts = text.lower().split("x")
    if len(parts) == 2 and parts[0].isdigit() and parts[1].isdigit():
        return (int(parts[0]), int(parts[1]), text)
    return (sys.maxsize, sys.maxsize, text)


def normalize_row(row: Dict[str, object]) -> Dict[str, object]:
    return {
        "resolution": str(row["resolution"]).strip(),
        "ssp": int(row["ssp"]),
        "depth": int(row["depth"]),
        "mpi": int(row["mpi"]),
        "openmp": int(row["openmp"]),
        "cores used": int(row["cores used"]),
        "time used in hours": round(float(row["time used in hours"]), 6),
    }


def row_key(row: Dict[str, object]) -> tuple[object, ...]:
    normalized = normalize_row(row)
    return (
        normalized["resolution"],
        normalized["ssp"],
        normalized["depth"],
        normalized["mpi"],
        normalized["openmp"],
        normalized["cores used"],
        f"{normalized['time used in hours']:.6f}",
    )


def sort_rows(rows: List[Dict[str, object]]) -> None:
    rows.sort(
        key=lambda row: (
            resolution_sort_key(row["resolution"]),
            int(row["ssp"]),
            int(row["depth"]),
            int(row["mpi"]),
            int(row["openmp"]),
        )
    )


def parse_reports(input_dir: Path) -> tuple[List[Dict[str, object]], List[Path], List[str]]:
    rows: List[Dict[str, object]] = []
    skipped_files: List[Path] = []
    warnings: List[str] = []

    for report_path in collect_report_files(input_dir):
        latest_metrics_text: str | None = None
        allocated_cpus: int | None = None

        try:
            with report_path.open("r", encoding="utf-8", errors="replace") as handle:
                for line in handle:
                    if latest_metrics_text is None:
                        latest_candidate = parse_latest_metrics_row(line)
                        if latest_candidate is not None:
                            latest_metrics_text = latest_candidate

                    if allocated_cpus is None:
                        cpu_candidate = parse_allocated_cpus(line)
                        if cpu_candidate is not None:
                            allocated_cpus = cpu_candidate
        except OSError as exc:
            warnings.append(f"Could not read file {report_path}: {exc}")
            skipped_files.append(report_path)
            continue

        if latest_metrics_text is None:
            skipped_files.append(report_path)
            continue

        try:
            fields = next(csv.reader([latest_metrics_text]))
        except Exception as exc:
            warnings.append(f"CSV parse failed in {report_path}: {exc}")
            skipped_files.append(report_path)
            continue

        if len(fields) < 12:
            warnings.append(
                f"Malformed Latest Metrics Row in {report_path} (expected at least 12 fields)."
            )
            skipped_files.append(report_path)
            continue

        try:
            mode = fields[3].strip().lower()
            if mode != "parallel":
                continue

            resolution = fields[5].strip()
            ssp = int(fields[6])
            depth = int(fields[7])
            mpi = int(fields[8])
            openmp = int(fields[9])
            cores_used = mpi * openmp
            time_used_hours = round(float(fields[11]) / 3600.0, 6)
        except (ValueError, IndexError) as exc:
            warnings.append(f"Value parse failed in {report_path}: {exc}")
            skipped_files.append(report_path)
            continue

        if allocated_cpus is not None and allocated_cpus != cores_used:
            warnings.append(
                f"Allocated CPU mismatch in {report_path}: allocated={allocated_cpus}, "
                f"mpi*openmp={cores_used}"
            )

        rows.append(
            {
                "resolution": resolution,
                "ssp": ssp,
                "depth": depth,
                "mpi": mpi,
                "openmp": openmp,
                "cores used": cores_used,
                "time used in hours": time_used_hours,
            }
        )

    rows = [normalize_row(row) for row in rows]
    sort_rows(rows)
    return rows, skipped_files, warnings


def read_existing_rows(csv_path: Path) -> tuple[List[Dict[str, object]], List[str]]:
    rows: List[Dict[str, object]] = []
    warnings: List[str] = []
    if not csv_path.exists():
        return rows, warnings

    try:
        with csv_path.open("r", newline="", encoding="utf-8") as handle:
            reader = csv.DictReader(handle)
            for idx, raw_row in enumerate(reader, start=2):
                try:
                    rows.append(
                        normalize_row(
                            {
                                "resolution": raw_row["resolution"],
                                "ssp": raw_row["ssp"],
                                "depth": raw_row["depth"],
                                "mpi": raw_row["mpi"],
                                "openmp": raw_row["openmp"],
                                "cores used": raw_row["cores used"],
                                "time used in hours": raw_row["time used in hours"],
                            }
                        )
                    )
                except Exception as exc:
                    warnings.append(
                        f"Skipping malformed existing CSV row {idx} in {csv_path}: {exc}"
                    )
    except OSError as exc:
        warnings.append(f"Could not read existing CSV {csv_path}: {exc}")
    return rows, warnings


def merge_unique_rows(
    existing_rows: List[Dict[str, object]], new_rows: List[Dict[str, object]]
) -> tuple[List[Dict[str, object]], int]:
    merged = [normalize_row(row) for row in existing_rows]
    seen = {row_key(row) for row in merged}
    appended = 0

    for row in new_rows:
        normalized = normalize_row(row)
        key = row_key(normalized)
        if key in seen:
            continue
        merged.append(normalized)
        seen.add(key)
        appended += 1

    sort_rows(merged)
    return merged, appended


def write_csv(rows: List[Dict[str, object]], csv_path: Path) -> None:
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=COLUMNS)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def print_table(rows: List[Dict[str, object]]) -> None:
    if not rows:
        print("(no rows parsed)")
        return

    widths = {column: len(column) for column in COLUMNS}
    for row in rows:
        for column in COLUMNS:
            value = row[column]
            text = f"{value:.6f}" if column == "time used in hours" else str(value)
            widths[column] = max(widths[column], len(text))

    header = "  ".join(column.ljust(widths[column]) for column in COLUMNS)
    print(header)
    print("  ".join("-" * widths[column] for column in COLUMNS))
    for row in rows:
        rendered = []
        for column in COLUMNS:
            value = row[column]
            text = f"{value:.6f}" if column == "time used in hours" else str(value)
            rendered.append(text.ljust(widths[column]))
        print("  ".join(rendered))


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Parse ToriRender txt/log run reports into a clean summary CSV table."
    )
    parser.add_argument(
        "--input",
        required=True,
        help="Folder containing report .txt/.log files (searched recursively).",
    )
    parser.add_argument(
        "--output",
        required=True,
        help="Output folder for torirender_parallel_summary.csv.",
    )
    args = parser.parse_args()

    input_dir = Path(args.input).expanduser().resolve()
    output_dir = Path(args.output).expanduser().resolve()

    if not input_dir.exists() or not input_dir.is_dir():
        print(f"Input folder does not exist or is not a directory: {input_dir}", file=sys.stderr)
        return 2

    rows, skipped_files, warnings = parse_reports(input_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    csv_path = output_dir / "torirender_parallel_summary.csv"

    existing_rows, existing_warnings = read_existing_rows(csv_path)
    warnings.extend(existing_warnings)
    merged_rows, appended_count = merge_unique_rows(existing_rows, rows)
    write_csv(merged_rows, csv_path)

    if skipped_files:
        print("Warning: skipped files (missing Latest Metrics Row or parse/read failure):")
        for path in skipped_files:
            print(f"  - {path}")

    if warnings:
        print("Warnings:")
        for warning in warnings:
            print(f"  - {warning}")

    print(
        f"\nAppend summary: existing={len(existing_rows)}, "
        f"parsed_now={len(rows)}, appended_new={appended_count}, final_total={len(merged_rows)}"
    )

    print("\nFinal Table:")
    print_table(merged_rows)
    print(f"\nSaved CSV: {csv_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
