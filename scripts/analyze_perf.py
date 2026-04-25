#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import math
import statistics
import struct
import zlib
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Callable, Dict, Iterable, List, Sequence, Tuple


KEY_COLS = ("scene_file", "image_width", "image_height", "samples_per_pixel", "max_depth")
PLOT_SIZE = (1200, 760)


def to_float(value, default: float = 0.0) -> float:
    if value is None:
        return default
    text = str(value).strip()
    if text == "":
        return default
    try:
        out = float(text)
        if math.isnan(out) or math.isinf(out):
            return default
        return out
    except Exception:
        return default


def to_int(value, default: int = 0) -> int:
    if value is None:
        return default
    text = str(value).strip()
    if text == "":
        return default
    try:
        return int(float(text))
    except Exception:
        return default


def parse_timestamp(value: str) -> datetime:
    value = (value or "").strip()
    for fmt in ("%Y-%m-%d_time_%Hh%Mm%Ss", "%Y-%m-%d %H:%M:%S"):
        try:
            return datetime.strptime(value, fmt)
        except Exception:
            pass
    return datetime.min


def discover_csv_files(input_path: Path) -> List[Path]:
    if input_path.is_dir():
        root = input_path
    else:
        root = input_path.parent
    files = sorted(root.glob("*.csv"))
    out = []
    for p in files:
        if p.name in {"performance_analysis.csv"}:
            continue
        if p.name.startswith("performance_"):
            continue
        if p.name == "resource_metrics.csv":
            continue
        out.append(p)
    return out


def read_rows(csv_files: Iterable[Path]) -> List[Dict[str, str]]:
    rows: List[Dict[str, str]] = []
    for path in csv_files:
        try:
            with path.open("r", newline="") as handle:
                reader = csv.DictReader(handle)
                if reader.fieldnames is None or "total_wall_seconds" not in reader.fieldnames:
                    continue
                for row in reader:
                    row = dict(row)
                    row["__source_csv"] = str(path)
                    rows.append(row)
        except Exception:
            continue
    return rows


def ensure_defaults(row: Dict[str, str]) -> None:
    defaults = {
        "run_id": "",
        "timestamp": "",
        "backend": "",
        "mode": "",
        "run_label": "",
        "image_width": "0",
        "image_height": "0",
        "resolution": "",
        "samples_per_pixel": "0",
        "max_depth": "0",
        "mpi_ranks": "1",
        "omp_threads": "1",
        "p_effective": "1",
        "scene_file": "",
        "output_file": "",
        "git_commit_if_available": "unknown",
        "total_wall_seconds": "0",
        "sigma_setup_seconds": "0",
        "scene_parse_seconds": "0",
        "bvh_build_seconds": "0",
        "camera_setup_seconds": "0",
        "mpi_init_seconds": "0",
        "mpi_broadcast_seconds": "0",
        "mpi_scatter_or_task_distribution_seconds": "0",
        "render_region_wall_seconds": "0",
        "omp_parallel_region_seconds": "0",
        "tile_compute_sum_seconds": "0",
        "tile_compute_max_seconds": "0",
        "tile_compute_min_seconds": "0",
        "mpi_gather_seconds": "0",
        "output_write_seconds": "0",
        "synchronization_seconds": "0",
        "finalization_seconds": "0",
        "max_rank_compute_seconds": "",
        "mean_rank_compute_seconds": "",
        "min_rank_compute_seconds": "",
        "load_imbalance_seconds": "",
        "load_imbalance_ratio": "",
        "communication_overhead_seconds": "",
        "synchronization_overhead_seconds": "",
        "scheduling_overhead_seconds": "",
        "output_overhead_seconds": "",
        "other_overhead_seconds": "",
        "Ts_serial_baseline_seconds": "",
        "serial_baseline_found": "",
        "serial_config_mismatch": "",
    }
    for k, v in defaults.items():
        row.setdefault(k, v)


def row_key(row: Dict[str, str]) -> Tuple[str, int, int, int, int]:
    return (
        row.get("scene_file", ""),
        to_int(row.get("image_width", 0), 0),
        to_int(row.get("image_height", 0), 0),
        to_int(row.get("samples_per_pixel", 0), 0),
        to_int(row.get("max_depth", 0), 0),
    )


def add_validation(rows: Sequence[Dict[str, str]], warnings: List[str]) -> None:
    for row in rows:
        run_id = row.get("run_id", "unknown")
        p_eff = to_int(row.get("p_effective", 0), 0)
        total = to_float(row.get("total_wall_seconds", 0.0), 0.0)
        speedup = to_float(row.get("speedup", 0.0), 0.0)
        eff = to_float(row.get("efficiency", 0.0), 0.0)
        kappa = to_float(row.get("kappa_estimated_seconds", 0.0), 0.0)
        baseline = to_float(row.get("Ts_serial_baseline_seconds", 0.0), 0.0)

        if p_eff <= 0:
            warnings.append(f"p_effective missing/invalid for run_id={run_id}.")
        if total <= 0:
            warnings.append(f"total_wall_seconds <= 0 for run_id={run_id}.")
        if speedup > (max(p_eff, 1) * 1.05):
            warnings.append(f"Suspicious speedup > p_effective for run_id={run_id}.")
        if total > 0 and kappa < min(-0.5, -0.05 * total):
            warnings.append(f"Strongly negative kappa for run_id={run_id}.")
        if eff > 1.05:
            warnings.append(f"efficiency > 1.05 for run_id={run_id}.")
        if row.get("mode", "").lower() != "serial" and baseline <= 0:
            warnings.append(f"No matching serial baseline for run_id={run_id}.")
        if row.get("mode", "").lower() != "serial" and row.get("serial_config_mismatch", "0") == "1":
            warnings.append(
                f"Serial and parallel run configs do not match exactly for run_id={run_id}."
            )


def compute_metrics(rows: List[Dict[str, str]], warnings: List[str]) -> List[Dict[str, str]]:
    for row in rows:
        ensure_defaults(row)
        row["mode"] = row.get("mode", "").lower()
        row["p_effective"] = str(max(1, to_int(row.get("p_effective", 1), 1)))
        row["timestamp_dt"] = parse_timestamp(row.get("timestamp", ""))

    serial_by_key: Dict[Tuple[str, int, int, int, int], List[Dict[str, str]]] = {}
    serial_keys_by_scene: Dict[str, set[Tuple[str, int, int, int, int]]] = {}
    for row in rows:
        if row["mode"] != "serial":
            continue
        key = row_key(row)
        serial_by_key.setdefault(key, []).append(row)
        scene = row.get("scene_file", "")
        serial_keys_by_scene.setdefault(scene, set()).add(key)

    for serial_rows in serial_by_key.values():
        serial_rows.sort(key=lambda r: r["timestamp_dt"])

    for row in rows:
        p_eff = float(to_int(row["p_effective"], 1))
        total = to_float(row["total_wall_seconds"], 0.0)
        sigma_setup = to_float(row["sigma_setup_seconds"], 0.0)

        if row["mode"] == "serial":
            baseline = total
            row["serial_baseline_found"] = "1"
        else:
            baseline = to_float(row.get("Ts_serial_baseline_seconds", ""), 0.0)
            if baseline <= 0:
                candidates = serial_by_key.get(row_key(row), [])
                if candidates:
                    baseline = to_float(candidates[-1].get("total_wall_seconds", 0.0), 0.0)
                    row["serial_baseline_found"] = "1"
                else:
                    row["serial_baseline_found"] = "0"
                    row["serial_config_mismatch"] = (
                        "1" if row.get("scene_file", "") in serial_keys_by_scene else "0"
                    )
            else:
                row["serial_baseline_found"] = "1"
                row["serial_config_mismatch"] = "0"
        if row["mode"] == "serial":
            row["serial_config_mismatch"] = "0"
        elif "serial_config_mismatch" not in row or row["serial_config_mismatch"] == "":
            row["serial_config_mismatch"] = "0"

        row["Ts_serial_baseline_seconds"] = f"{baseline:.9f}" if baseline > 0 else ""

        max_rank_compute = to_float(row.get("max_rank_compute_seconds", ""), 0.0)
        mean_rank_compute = to_float(row.get("mean_rank_compute_seconds", ""), 0.0)
        min_rank_compute = to_float(row.get("min_rank_compute_seconds", ""), 0.0)
        render_region = to_float(row.get("render_region_wall_seconds", ""), 0.0)
        if max_rank_compute <= 0:
            max_rank_compute = render_region
        if mean_rank_compute <= 0:
            mean_rank_compute = max_rank_compute
        if min_rank_compute <= 0:
            min_rank_compute = mean_rank_compute

        mpi_bcast = to_float(row.get("mpi_broadcast_seconds", ""), 0.0)
        mpi_scatter = to_float(row.get("mpi_scatter_or_task_distribution_seconds", ""), 0.0)
        mpi_gather = to_float(row.get("mpi_gather_seconds", ""), 0.0)
        sync = to_float(row.get("synchronization_seconds", ""), 0.0)
        omp_region = to_float(row.get("omp_parallel_region_seconds", ""), 0.0)
        tile_sum = to_float(row.get("tile_compute_sum_seconds", ""), 0.0)
        output = to_float(row.get("output_write_seconds", ""), 0.0)

        comm_overhead = to_float(row.get("communication_overhead_seconds", ""), math.nan)
        if math.isnan(comm_overhead):
            comm_overhead = mpi_bcast + mpi_scatter + mpi_gather
        sync_overhead = to_float(row.get("synchronization_overhead_seconds", ""), math.nan)
        if math.isnan(sync_overhead):
            sync_overhead = sync
        sched_overhead = to_float(row.get("scheduling_overhead_seconds", ""), math.nan)
        if math.isnan(sched_overhead):
            sched_overhead = max(0.0, omp_region - tile_sum)
        load_imbalance = to_float(row.get("load_imbalance_seconds", ""), math.nan)
        if math.isnan(load_imbalance):
            load_imbalance = max(0.0, max_rank_compute - mean_rank_compute)
        output_overhead = to_float(row.get("output_overhead_seconds", ""), math.nan)
        if math.isnan(output_overhead):
            output_overhead = output

        sigma = sigma_setup
        phi_serial = baseline - sigma if baseline > 0 else 0.0
        ideal_phi_over_p = phi_serial / p_eff if p_eff > 0 else 0.0
        kappa_raw = total - sigma - ideal_phi_over_p if baseline > 0 else 0.0
        kappa_clamped = max(0.0, kappa_raw)
        tp_model = sigma + ideal_phi_over_p + kappa_raw
        overhead_fraction = (kappa_raw / total) if total > 0 else 0.0
        sigma_fraction = (sigma / baseline) if baseline > 0 else 0.0
        phi_fraction = (phi_serial / baseline) if baseline > 0 else 0.0
        speedup = (baseline / total) if total > 0 and baseline > 0 else 0.0
        efficiency = speedup / p_eff if p_eff > 0 else 0.0
        f_serial = sigma / baseline if baseline > 0 else 0.0
        amdahl = (1.0 / (f_serial + (1.0 - f_serial) / p_eff)) if p_eff > 0 else 0.0
        karp = (
            ((1.0 / speedup) - (1.0 / p_eff)) / (1.0 - (1.0 / p_eff))
            if (p_eff > 1 and speedup > 0)
            else 0.0
        )
        eff_loss = 1.0 - efficiency
        other_overhead = (
            kappa_raw - comm_overhead - sync_overhead - sched_overhead - load_imbalance - output_overhead
        )

        row["max_rank_compute_seconds"] = f"{max_rank_compute:.9f}"
        row["mean_rank_compute_seconds"] = f"{mean_rank_compute:.9f}"
        row["min_rank_compute_seconds"] = f"{min_rank_compute:.9f}"
        row["load_imbalance_seconds"] = f"{load_imbalance:.9f}"
        row["load_imbalance_ratio"] = f"{(max_rank_compute / mean_rank_compute) if mean_rank_compute > 0 else 0.0:.9f}"
        row["communication_overhead_seconds"] = f"{comm_overhead:.9f}"
        row["synchronization_overhead_seconds"] = f"{sync_overhead:.9f}"
        row["scheduling_overhead_seconds"] = f"{sched_overhead:.9f}"
        row["output_overhead_seconds"] = f"{output_overhead:.9f}"
        row["other_overhead_seconds"] = f"{other_overhead:.9f}"
        row["sigma_seconds"] = f"{sigma:.9f}"
        row["phi_serial_seconds"] = f"{phi_serial:.9f}"
        row["ideal_phi_over_p_seconds"] = f"{ideal_phi_over_p:.9f}"
        row["kappa_estimated_seconds"] = f"{kappa_raw:.9f}"
        row["kappa_estimated_clamped_seconds"] = f"{kappa_clamped:.9f}"
        row["Tp_model_reconstructed_seconds"] = f"{tp_model:.9f}"
        row["overhead_fraction_of_Tp"] = f"{overhead_fraction:.9f}"
        row["sigma_fraction_of_Ts"] = f"{sigma_fraction:.9f}"
        row["phi_fraction_of_Ts"] = f"{phi_fraction:.9f}"
        row["speedup"] = f"{speedup:.9f}"
        row["efficiency"] = f"{efficiency:.9f}"
        row["amdahl_ideal_speedup_from_measured_sigma"] = f"{amdahl:.9f}"
        row["karp_flatt_e"] = f"{karp:.9f}"
        row["parallel_efficiency_loss"] = f"{eff_loss:.9f}"

    add_validation(rows, warnings)
    rows.sort(key=lambda r: r["timestamp_dt"])
    return rows


def write_csv(rows: List[Dict[str, str]], out_path: Path) -> None:
    if not rows:
        with out_path.open("w", newline="") as handle:
            handle.write("")
        return

    header_order = [
        "run_id",
        "timestamp",
        "backend",
        "mode",
        "run_label",
        "image_width",
        "image_height",
        "resolution",
        "samples_per_pixel",
        "max_depth",
        "mpi_ranks",
        "omp_threads",
        "p_effective",
        "scene_file",
        "output_file",
        "git_commit_if_available",
        "total_wall_seconds",
        "sigma_setup_seconds",
        "scene_parse_seconds",
        "bvh_build_seconds",
        "camera_setup_seconds",
        "mpi_init_seconds",
        "mpi_broadcast_seconds",
        "mpi_scatter_or_task_distribution_seconds",
        "render_region_wall_seconds",
        "omp_parallel_region_seconds",
        "tile_compute_sum_seconds",
        "tile_compute_max_seconds",
        "tile_compute_min_seconds",
        "mpi_gather_seconds",
        "output_write_seconds",
        "synchronization_seconds",
        "finalization_seconds",
        "max_rank_compute_seconds",
        "mean_rank_compute_seconds",
        "min_rank_compute_seconds",
        "load_imbalance_seconds",
        "load_imbalance_ratio",
        "communication_overhead_seconds",
        "synchronization_overhead_seconds",
        "scheduling_overhead_seconds",
        "output_overhead_seconds",
        "other_overhead_seconds",
        "Ts_serial_baseline_seconds",
        "sigma_seconds",
        "phi_serial_seconds",
        "ideal_phi_over_p_seconds",
        "kappa_estimated_seconds",
        "kappa_estimated_clamped_seconds",
        "Tp_model_reconstructed_seconds",
        "overhead_fraction_of_Tp",
        "sigma_fraction_of_Ts",
        "phi_fraction_of_Ts",
        "speedup",
        "efficiency",
        "amdahl_ideal_speedup_from_measured_sigma",
        "karp_flatt_e",
        "parallel_efficiency_loss",
        "serial_config_mismatch",
        "serial_baseline_found",
        "__source_csv",
    ]
    available = set().union(*(r.keys() for r in rows))
    available.discard("timestamp_dt")
    header = [h for h in header_order if h in available]
    extra = sorted(k for k in available if k not in header)
    header.extend(extra)

    with out_path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=header)
        writer.writeheader()
        for row in rows:
            out = {k: v for k, v in row.items() if k != "timestamp_dt"}
            writer.writerow(out)


def group_parallel(rows: List[Dict[str, str]]) -> Dict[int, List[Dict[str, str]]]:
    grouped: Dict[int, List[Dict[str, str]]] = {}
    for row in rows:
        if row.get("mode", "").lower() == "serial":
            continue
        p = max(1, to_int(row.get("p_effective", 1), 1))
        grouped.setdefault(p, []).append(row)
    return dict(sorted(grouped.items()))


def mean_metric(rows: Sequence[Dict[str, str]], key: str) -> float:
    values = [to_float(r.get(key, 0.0), 0.0) for r in rows]
    if not values:
        return 0.0
    return float(statistics.fmean(values))


@dataclass
class Canvas:
    width: int
    height: int

    def __post_init__(self) -> None:
        self.row_stride = self.width * 3
        self.data = bytearray([255] * (self.height * self.row_stride))

    def _clip(self, x: int, y: int) -> Tuple[int, int]:
        return max(0, min(self.width - 1, x)), max(0, min(self.height - 1, y))

    def set_px(self, x: int, y: int, color: Tuple[int, int, int]) -> None:
        if 0 <= x < self.width and 0 <= y < self.height:
            offset = y * self.row_stride + x * 3
            self.data[offset] = color[0]
            self.data[offset + 1] = color[1]
            self.data[offset + 2] = color[2]

    def rect(self, x0: int, y0: int, x1: int, y1: int, color: Tuple[int, int, int]) -> None:
        xa, xb = sorted((x0, x1))
        ya, yb = sorted((y0, y1))
        xa = max(0, xa)
        xb = min(self.width - 1, xb)
        ya = max(0, ya)
        yb = min(self.height - 1, yb)
        if xa > xb or ya > yb:
            return
        for y in range(ya, yb + 1):
            row_base = y * self.row_stride
            for x in range(xa, xb + 1):
                offset = row_base + x * 3
                self.data[offset] = color[0]
                self.data[offset + 1] = color[1]
                self.data[offset + 2] = color[2]

    def line(self, x0: int, y0: int, x1: int, y1: int, color: Tuple[int, int, int]) -> None:
        dx = abs(x1 - x0)
        sx = 1 if x0 < x1 else -1
        dy = -abs(y1 - y0)
        sy = 1 if y0 < y1 else -1
        err = dx + dy
        while True:
            self.set_px(x0, y0, color)
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 >= dy:
                err += dy
                x0 += sx
            if e2 <= dx:
                err += dx
                y0 += sy

    def polyline(self, points: Sequence[Tuple[int, int]], color: Tuple[int, int, int]) -> None:
        if len(points) < 2:
            return
        for i in range(1, len(points)):
            self.line(points[i - 1][0], points[i - 1][1], points[i][0], points[i][1], color)

    def circle(self, cx: int, cy: int, r: int, color: Tuple[int, int, int]) -> None:
        for y in range(cy - r, cy + r + 1):
            for x in range(cx - r, cx + r + 1):
                if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                    self.set_px(x, y, color)

    def save_png(self, path: Path) -> None:
        raw = bytearray()
        for y in range(self.height):
            raw.append(0)  # no filter
            row_start = y * self.row_stride
            row_end = row_start + self.row_stride
            raw.extend(self.data[row_start:row_end])
        comp = zlib.compress(bytes(raw), level=9)

        def chunk(tag: bytes, payload: bytes) -> bytes:
            return (
                struct.pack(">I", len(payload))
                + tag
                + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
            )

        ihdr = struct.pack(">IIBBBBB", self.width, self.height, 8, 2, 0, 0, 0)
        png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", comp) + chunk(b"IEND", b"")
        path.write_bytes(png)


def axis_transform(
    x_values: Sequence[float],
    y_values: Sequence[float],
    width: int,
    height: int,
    margins: Tuple[int, int, int, int] = (80, 30, 40, 70),
) -> Tuple[Callable[[float], int], Callable[[float], int]]:
    left, right, top, bottom = margins
    x_min = min(x_values) if x_values else 0.0
    x_max = max(x_values) if x_values else 1.0
    y_min = 0.0
    y_max = max(y_values) if y_values else 1.0
    if abs(x_max - x_min) < 1e-12:
        x_max = x_min + 1.0
    if y_max <= 0:
        y_max = 1.0

    def px(x: float) -> int:
        return int(left + (x - x_min) / (x_max - x_min) * (width - left - right))

    def py(y: float) -> int:
        return int(height - bottom - (y - y_min) / (y_max - y_min) * (height - top - bottom))

    return px, py


def draw_axes(canvas: Canvas, left: int = 80, right: int = 30, top: int = 40, bottom: int = 70) -> None:
    x0 = left
    y0 = canvas.height - bottom
    x1 = canvas.width - right
    y1 = top
    canvas.line(x0, y0, x1, y0, (0, 0, 0))
    canvas.line(x0, y0, x0, y1, (0, 0, 0))


def plot_stacked_bar(
    out_path: Path,
    x_vals: Sequence[int],
    stacks: Sequence[Tuple[str, Sequence[float], Tuple[int, int, int]]],
) -> None:
    canvas = Canvas(*PLOT_SIZE)
    draw_axes(canvas)
    left, right, top, bottom = 80, 30, 40, 70
    plot_w = canvas.width - left - right
    plot_h = canvas.height - top - bottom

    x_count = max(len(x_vals), 1)
    bar_w = max(8, int(plot_w / (x_count * 1.5)))
    totals = [0.0 for _ in x_vals]
    for _, vals, _ in stacks:
        for i, v in enumerate(vals):
            totals[i] += max(0.0, v)
    y_max = max(totals) if totals else 1.0
    if y_max <= 0:
        y_max = 1.0

    for i, _x in enumerate(x_vals):
        cx = left + int((i + 0.5) * plot_w / x_count)
        base = 0.0
        for _, vals, color in stacks:
            v = max(0.0, vals[i])
            y0 = canvas.height - bottom - int(base / y_max * plot_h)
            base += v
            y1 = canvas.height - bottom - int(base / y_max * plot_h)
            canvas.rect(cx - bar_w // 2, y1, cx + bar_w // 2, y0, color)

    canvas.save_png(out_path)


def plot_line(
    out_path: Path,
    x_vals: Sequence[float],
    lines: Sequence[Tuple[Sequence[float], Tuple[int, int, int]]],
    highlight: Tuple[float, float] | None = None,
) -> None:
    canvas = Canvas(*PLOT_SIZE)
    draw_axes(canvas)
    all_y: List[float] = []
    for ys, _ in lines:
        all_y.extend(ys)
    if highlight is not None:
        all_y.append(highlight[1])
    px, py = axis_transform(x_vals, all_y, canvas.width, canvas.height)

    for ys, color in lines:
        points = [(px(float(x_vals[i])), py(float(ys[i]))) for i in range(min(len(x_vals), len(ys)))]
        canvas.polyline(points, color)
        for x, y in points:
            canvas.circle(x, y, 3, color)

    if highlight is not None:
        hx, hy = highlight
        canvas.circle(px(hx), py(hy), 6, (220, 30, 30))

    canvas.save_png(out_path)


def save_plots(rows: List[Dict[str, str]], outdir: Path) -> None:
    plots_dir = outdir / "plots"
    plots_dir.mkdir(parents=True, exist_ok=True)
    grouped = group_parallel(rows)
    if not grouped:
        # Emit blank placeholders so required files still exist.
        blank = Canvas(*PLOT_SIZE)
        for name in [
            "time_decomposition_stacked_bar.png",
            "overhead_breakdown_stacked_bar.png",
            "speedup_vs_processors.png",
            "efficiency_vs_processors.png",
            "karp_flatt_vs_processors.png",
            "total_time_vs_processors.png",
        ]:
            blank.save_png(plots_dir / name)
        return

    p_vals = sorted(grouped.keys())

    sigma = [mean_metric(grouped[p], "sigma_seconds") for p in p_vals]
    ideal = [mean_metric(grouped[p], "ideal_phi_over_p_seconds") for p in p_vals]
    kappa = [mean_metric(grouped[p], "kappa_estimated_clamped_seconds") for p in p_vals]
    plot_stacked_bar(
        plots_dir / "time_decomposition_stacked_bar.png",
        p_vals,
        [
            ("sigma", sigma, (66, 133, 244)),
            ("ideal_phi_over_p", ideal, (15, 157, 88)),
            ("kappa", kappa, (255, 140, 0)),
        ],
    )

    comm = [mean_metric(grouped[p], "communication_overhead_seconds") for p in p_vals]
    sync = [mean_metric(grouped[p], "synchronization_overhead_seconds") for p in p_vals]
    sched = [mean_metric(grouped[p], "scheduling_overhead_seconds") for p in p_vals]
    load = [mean_metric(grouped[p], "load_imbalance_seconds") for p in p_vals]
    outp = [mean_metric(grouped[p], "output_overhead_seconds") for p in p_vals]
    other = [mean_metric(grouped[p], "other_overhead_seconds") for p in p_vals]
    plot_stacked_bar(
        plots_dir / "overhead_breakdown_stacked_bar.png",
        p_vals,
        [
            ("communication", comm, (66, 133, 244)),
            ("synchronization", sync, (240, 80, 80)),
            ("scheduling", sched, (250, 180, 20)),
            ("load_imbalance", load, (140, 90, 220)),
            ("output", outp, (15, 157, 88)),
            ("other", other, (90, 90, 90)),
        ],
    )

    speedup = [mean_metric(grouped[p], "speedup") for p in p_vals]
    ideal_speedup = [float(p) for p in p_vals]
    sigma_samples = [
        to_float(r.get("sigma_fraction_of_Ts", 0.0), 0.0)
        for r in rows
        if to_float(r.get("sigma_fraction_of_Ts", 0.0), 0.0) > 0
    ]
    sigma_fraction_ref = statistics.fmean(sigma_samples) if sigma_samples else 0.0
    amdahl = [1.0 / (sigma_fraction_ref + (1.0 - sigma_fraction_ref) / float(p)) for p in p_vals]
    plot_line(
        plots_dir / "speedup_vs_processors.png",
        [float(p) for p in p_vals],
        [
            (speedup, (66, 133, 244)),
            (ideal_speedup, (160, 160, 160)),
            (amdahl, (15, 157, 88)),
        ],
    )

    efficiency = [mean_metric(grouped[p], "efficiency") for p in p_vals]
    plot_line(
        plots_dir / "efficiency_vs_processors.png",
        [float(p) for p in p_vals],
        [(efficiency, (66, 133, 244))],
    )

    karp = [mean_metric(grouped[p], "karp_flatt_e") for p in p_vals]
    plot_line(
        plots_dir / "karp_flatt_vs_processors.png",
        [float(p) for p in p_vals],
        [(karp, (66, 133, 244))],
    )

    total = [mean_metric(grouped[p], "total_wall_seconds") for p in p_vals]
    best_idx = min(range(len(total)), key=lambda idx: total[idx])
    plot_line(
        plots_dir / "total_time_vs_processors.png",
        [float(p) for p in p_vals],
        [(total, (66, 133, 244))],
        highlight=(float(p_vals[best_idx]), float(total[best_idx])),
    )


def write_summary(rows: List[Dict[str, str]], warnings: List[str], outdir: Path) -> None:
    out_path = outdir / "performance_summary.md"
    grouped = group_parallel(rows)
    if not grouped:
        out_path.write_text("# ToriRender Performance Summary\n\nNo parallel rows found.\n")
        return

    p_vals = sorted(grouped.keys())
    total_by_p = {p: mean_metric(grouped[p], "total_wall_seconds") for p in p_vals}
    speedup_by_p = {p: mean_metric(grouped[p], "speedup") for p in p_vals}
    eff_by_p = {p: mean_metric(grouped[p], "efficiency") for p in p_vals}
    karp_by_p = {p: mean_metric(grouped[p], "karp_flatt_e") for p in p_vals}

    best_p = min(total_by_p, key=lambda p: total_by_p[p])
    max_speedup_p = max(speedup_by_p, key=lambda p: speedup_by_p[p])
    sigma_mean = statistics.fmean(
        [to_float(r.get("sigma_fraction_of_Ts", 0.0), 0.0) for r in rows if to_float(r.get("sigma_fraction_of_Ts", 0.0), 0.0) > 0]
        or [0.0]
    )
    kappa_mean = statistics.fmean(
        [to_float(r.get("overhead_fraction_of_Tp", 0.0), 0.0) for r in rows]
        or [0.0]
    )
    limiter = "sigma" if sigma_mean > kappa_mean else "kappa"

    # Pick representative row at best p
    best_rows = grouped[best_p]
    rep = min(best_rows, key=lambda r: to_float(r.get("total_wall_seconds", 0.0), 0.0))

    lines = [
        "# ToriRender Performance Summary",
        "",
        f"- Best `p_effective`: **{best_p}** with minimum runtime **{total_by_p[best_p]:.6f}s**.",
        f"- Maximum measured speedup: **{speedup_by_p[max_speedup_p]:.6f}x** at `p_effective={max_speedup_p}`.",
        f"- Scaling is more limited by **{limiter}** (mean sigma fraction={sigma_mean:.4f}, mean kappa fraction={kappa_mean:.4f}).",
        "",
        "## Efficiency at Each p",
        "",
    ]
    for p in p_vals:
        lines.append(f"- p={p}: efficiency={eff_by_p[p]:.6f}")
    lines.extend(["", "## Karp-Flatt Trend", ""])
    for p in p_vals:
        lines.append(f"- p={p}: e={karp_by_p[p]:.6f}")
    lines.extend(
        [
            "",
            "## Model Interpretation",
            "",
            "Using `Tp = sigma + phi/p + kappa` with measured ToriRender values "
            f"at the best point (`p={best_p}`): "
            f"`Tp={to_float(rep.get('total_wall_seconds', 0.0), 0.0):.6f}s`, "
            f"`sigma={to_float(rep.get('sigma_seconds', 0.0), 0.0):.6f}s`, "
            f"`phi/p={to_float(rep.get('ideal_phi_over_p_seconds', 0.0), 0.0):.6f}s`, "
            f"`kappa={to_float(rep.get('kappa_estimated_seconds', 0.0), 0.0):.6f}s`.",
        ]
    )
    if warnings:
        lines.extend(["", "## Warnings", ""])
        for w in warnings:
            lines.append(f"- {w}")

    out_path.write_text("\n".join(lines) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser(description="Analyze ToriRender profiling metrics.")
    parser.add_argument(
        "--input",
        default="results/perf/metrics.csv",
        help="Input CSV path or directory (default: results/perf/metrics.csv).",
    )
    parser.add_argument(
        "--outdir",
        default="results/perf",
        help="Output directory for analysis CSV, summary, and plots.",
    )
    args = parser.parse_args()

    input_path = Path(args.input).expanduser().resolve()
    outdir = Path(args.outdir).expanduser().resolve()
    outdir.mkdir(parents=True, exist_ok=True)

    csv_files = discover_csv_files(input_path)
    if not csv_files and input_path.is_file():
        csv_files = [input_path]

    warnings: List[str] = []
    rows = read_rows(csv_files)
    if not rows:
        warnings.append("No valid profiling CSV rows were found.")
        write_csv([], outdir / "performance_analysis.csv")
        write_summary([], warnings, outdir)
        save_plots([], outdir)
        for w in warnings:
            print(f"Warning: {w}")
        return 0

    rows = compute_metrics(rows, warnings)
    write_csv(rows, outdir / "performance_analysis.csv")
    write_summary(rows, warnings, outdir)
    save_plots(rows, outdir)

    if warnings:
        print("Validation warnings:")
        for w in warnings:
            print(f"- {w}")
    else:
        print("Analysis completed without validation warnings.")
    print(f"Saved analysis CSV: {outdir / 'performance_analysis.csv'}")
    print(f"Saved summary markdown: {outdir / 'performance_summary.md'}")
    print(f"Saved plots directory: {outdir / 'plots'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
