#!/usr/bin/env python3
#  Copyright (c) Meta Platforms, Inc. and affiliates.
#  This source code is licensed under both the GPLv2 (found in the
#  COPYING file in the root directory) and Apache 2.0 License
#  (found in the LICENSE.Apache file in the root directory).
"""Generate per-metric KiWi filter performance plots from tile_filter.md.

Produces two complementary families of plots (one metric per plot file,
different lines representing different filters):
  1. vs_h_*.png:        x-axis = tile size h in {4, 8, 16}
  2. vs_split_h8_*.png: x-axis = delete-query split at fixed h (default h = 8)
"""

import argparse
from pathlib import Path
import re
import statistics

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import NullLocator, ScalarFormatter

TILE_SIZES = [4, 8, 16]
ALL_SPLITS = ["0-100", "25-75", "50-50", "75-25", "100-0"]
QUERY_SPLITS = ["0-100", "25-75", "50-50", "75-25"]
DELETE_SPLITS = ["25-75", "50-50", "75-25", "100-0"]

CANDIDATES = [
    "plain_bloom10",
    "kiwi_page_bloom10",
    "kiwi_tile_xor_baseline",
    "kiwi_sst_xor_baseline",
]

STYLE = {
    "plain_bloom10": {
        "label": "plain_bloom10 (non-KiWi, h=0 ref)",
        "color": "#5c677d",
        "linestyle": "--",
        "marker": "s",
        "linewidth": 2.1,
        "annot_offset": (0, 8),
    },
    "kiwi_page_bloom10": {
        "label": "kiwi_page_bloom10 (v1 page Bloom)",
        "color": "#d95f02",
        "linestyle": "-",
        "marker": "o",
        "linewidth": 2.3,
        "annot_offset": (0, 8),
    },
    "kiwi_tile_xor_baseline": {
        "label": "kiwi_tile_xor_baseline (v2 tile XOR)",
        "color": "#1f78b4",
        "linestyle": "-",
        "marker": "^",
        "linewidth": 2.3,
        "annot_offset": (0, 8),
    },
    "kiwi_sst_xor_baseline": {
        "label": "kiwi_sst_xor_baseline (v3 SST XOR)",
        "color": "#1b9e77",
        "linestyle": "-",
        "marker": "D",
        "linewidth": 2.5,
        "annot_offset": (0, -14),
    },
}


def parse_float_or_none(text: str):
    text = text.strip()
    if text == "N/A":
        return None
    return float(text)


def parse_benchmark_markdown(md_path: Path):
    """Return dict[(section_h, split, candidate)] -> metric dict."""
    data = {}
    current_h = None
    section_re = re.compile(r"^###\s+Tile Size\s+`h\s*=\s*(\d+)`")

    for raw_line in md_path.read_text(encoding="ascii").splitlines():
        line = raw_line.strip()
        sec_match = section_re.match(line)
        if sec_match:
            current_h = int(sec_match.group(1))
            continue
        if current_h is None or not line.startswith("|"):
            continue

        cells = [c.strip().strip("`") for c in line.split("|")[1:-1]]
        if len(cells) != 15 or cells[0] in ("h", "---") or cells[0].startswith("-"):
            continue

        split = cells[1]
        cand = cells[2]
        wall_ms = float(cells[3])
        total_kops = float(cells[4])
        raw_kops = float(cells[5])
        del_kkeys = parse_float_or_none(cells[6])

        rdel_s, rdel_ms = None, None
        if cells[7] != "N/A":
            m = re.match(r"([\d.]+)\s*\(([\d.]+)\)", cells[7])
            if not m:
                raise ValueError(f"Cannot parse Range Del cell: {cells[7]}")
            rdel_s, rdel_ms = float(m.group(1)), float(m.group(2))

        io_kqps, io_us, io_blk = None, None, None
        if cells[8] != "N/A":
            m = re.match(r"([\d.]+)\s*\(([\d.]+),\s*([\d.]+)\)", cells[8])
            if not m:
                raise ValueError(f"Cannot parse IO Get cell: {cells[8]}")
            io_kqps = float(m.group(1))
            io_us = float(m.group(2))
            io_blk = float(m.group(3))

        mem_kqps, mem_ns = None, None
        if cells[9] != "N/A":
            m = re.match(r"([\d.]+)\s*\(([\d.]+)\)", cells[9])
            if not m:
                raise ValueError(f"Cannot parse Mem Get cell: {cells[9]}")
            mem_kqps = float(m.group(1))
            mem_ns = float(m.group(2))

        m_w = re.match(r"([\d.]+)\s*\(([\d.]+)\)", cells[10])
        if not m_w:
            raise ValueError(f"Cannot parse w_amp cell: {cells[10]}")
        w_amp, wl_mb = float(m_w.group(1)), float(m_w.group(2))

        sp_amp = float(cells[11].rstrip("x"))
        flt_bpk, dir_bpk = [float(x.strip()) for x in cells[12].split("/")]
        tr_before, tr_after = [float(x.strip()) for x in cells[13].split("->")]

        m_sst = re.match(r"([\d.]+)->([\d.]+)\s*\(([\d.]+)\)", cells[14])
        if not m_sst:
            raise ValueError(f"Cannot parse sst_MB cell: {cells[14]}")
        sst_before = float(m_sst.group(1))
        sst_after = float(m_sst.group(2))
        sst_compact = float(m_sst.group(3))

        data[(current_h, split, cand)] = {
            "wall_ms": wall_ms,
            "total_kops": total_kops,
            "raw_kops": raw_kops,
            "del_kkeys": del_kkeys,
            "rdel_s": rdel_s,
            "rdel_ms": rdel_ms,
            "io_kqps": io_kqps,
            "io_us": io_us,
            "io_blk": io_blk,
            "mem_kqps": mem_kqps,
            "mem_ns": mem_ns,
            "w_amp": w_amp,
            "wl_mb": wl_mb,
            "sp_amp": sp_amp,
            "flt_bpk": flt_bpk,
            "dir_bpk": dir_bpk,
            "tr_before": tr_before,
            "tr_after": tr_after,
            "sst_before": sst_before,
            "sst_after": sst_after,
            "sst_compact": sst_compact,
        }

    return data


def style_h_axis(ax, ylabel: str | None = None):
    ax.set_xscale("log", base=2)
    ax.set_xticks(TILE_SIZES)
    ax.xaxis.set_major_formatter(ScalarFormatter())
    ax.xaxis.set_minor_locator(NullLocator())
    ax.set_xlim(3.4, 18.8)
    ax.set_xlabel("Tile size h (pages/tile)", fontsize=10)
    if ylabel:
        ax.set_ylabel(ylabel, fontsize=10)
    ax.grid(True, linestyle=":", linewidth=0.8, alpha=0.65)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)


def style_split_axis(ax, splits: list[str], ylabel: str):
    xs = list(range(len(splits)))
    ax.set_xticks(xs)
    ax.set_xticklabels(splits, fontsize=10)
    ax.set_xlim(-0.25, len(splits) - 0.75)
    ax.set_xlabel("Delete-Query Split (del% - query%)", fontsize=10.5)
    ax.set_ylabel(ylabel, fontsize=10.5)
    ax.grid(True, linestyle=":", linewidth=0.8, alpha=0.65)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)


def add_top_legend(fig, ax, y_anchor: float = 0.93, ncol: int = 2, fontsize: float = 9.8):
    handles, labels = ax.get_legend_handles_labels()
    fig.legend(
        handles,
        labels,
        loc="upper center",
        bbox_to_anchor=(0.5, y_anchor),
        ncol=ncol,
        frameon=True,
        fontsize=fontsize,
        columnspacing=1.6,
    )


def plot_multi_panel_vs_h(
    data,
    out_path: Path,
    title: str,
    ylabel: str,
    panels: list[tuple[str, callable]],
    nrows: int,
    ncols: int,
    figsize: tuple[float, float],
    zero_floor: bool = True,
):
    fig, axes = plt.subplots(nrows, ncols, figsize=figsize, squeeze=False)
    axes_flat = [ax for row in axes for ax in row]

    for idx, (panel_title, series_fn) in enumerate(panels):
        ax = axes_flat[idx]
        all_vals = []
        for cand in CANDIDATES:
            ys = [series_fn(data, h, cand) for h in TILE_SIZES]
            all_vals.extend(y for y in ys if y is not None)
            st = STYLE[cand]
            ax.plot(
                TILE_SIZES,
                ys,
                label=st["label"],
                color=st["color"],
                linestyle=st["linestyle"],
                marker=st["marker"],
                linewidth=st["linewidth"],
                markersize=6.5,
            )
        ax.set_title(panel_title, fontsize=11, fontweight="semibold", pad=7)
        style_h_axis(ax, ylabel if (idx % ncols == 0) else None)
        if all_vals:
            ymin, ymax = min(all_vals), max(all_vals)
            span = max(ymax - ymin, 0.05 * max(abs(ymax), 1.0))
            low = (
                0.0
                if (zero_floor and ymin >= 0 and ymin < 0.35 * ymax)
                else (ymin - 0.14 * span)
            )
            if zero_floor and ymin >= 0:
                low = max(0.0, low)
            ax.set_ylim(low, ymax + 0.16 * span)

    for extra_ax in axes_flat[len(panels) :]:
        extra_ax.set_visible(False)

    fig.suptitle(title, fontsize=13.5, fontweight="bold", y=0.985)
    add_top_legend(fig, axes_flat[0], y_anchor=0.935, ncol=2, fontsize=10)
    fig.tight_layout(rect=[0.0, 0.0, 1.0, 0.865])
    fig.savefig(out_path, dpi=180)
    plt.close(fig)


def plot_single_panel_vs_h(
    data,
    out_path: Path,
    title: str,
    ylabel: str,
    series_fn,
    candidates=CANDIDATES,
):
    fig, ax = plt.subplots(figsize=(7.6, 5.4))
    all_vals = []
    for cand in candidates:
        ys = [series_fn(data, h, cand) for h in TILE_SIZES]
        all_vals.extend(ys)
        st = STYLE[cand]
        ax.plot(
            TILE_SIZES,
            ys,
            label=st["label"],
            color=st["color"],
            linestyle=st["linestyle"],
            marker=st["marker"],
            linewidth=st["linewidth"],
            markersize=7.0,
        )
        for x, y in zip(TILE_SIZES, ys):
            ax.annotate(
                f"{y:.2f}",
                (x, y),
                textcoords="offset points",
                xytext=st["annot_offset"],
                ha="center",
                fontsize=8.6,
                color=st["color"],
            )

    style_h_axis(ax, ylabel)
    ymin, ymax = min(all_vals), max(all_vals)
    span = max(ymax - ymin, 1.0)
    ax.set_ylim(max(0.0, ymin - 0.18 * span), ymax + 0.20 * span)
    fig.suptitle(title, fontsize=12.5, fontweight="bold", y=0.98)
    add_top_legend(fig, ax, y_anchor=0.92, ncol=2, fontsize=8.9)
    fig.tight_layout(rect=[0.0, 0.0, 1.0, 0.82])
    fig.savefig(out_path, dpi=180)
    plt.close(fig)


def plot_single_panel_vs_split(
    data,
    out_path: Path,
    fixed_h: int,
    splits: list[str],
    metric_key: str,
    title: str,
    ylabel: str,
    zero_floor: bool = True,
):
    fig, ax = plt.subplots(figsize=(8.0, 5.4))
    xs = list(range(len(splits)))
    all_vals = []

    for cand in CANDIDATES:
        ys = [data[(fixed_h, s, cand)][metric_key] for s in splits]
        all_vals.extend(y for y in ys if y is not None)
        st = STYLE[cand]
        ax.plot(
            xs,
            ys,
            label=st["label"],
            color=st["color"],
            linestyle=st["linestyle"],
            marker=st["marker"],
            linewidth=st["linewidth"],
            markersize=7.0,
        )

    style_split_axis(ax, splits, ylabel)
    if all_vals:
        ymin, ymax = min(all_vals), max(all_vals)
        span = max(ymax - ymin, 0.05 * max(abs(ymax), 1.0))
        low = (
            0.0
            if (zero_floor and ymin >= 0 and ymin < 0.35 * ymax)
            else (ymin - 0.14 * span)
        )
        if zero_floor and ymin >= 0:
            low = max(0.0, low)
        ax.set_ylim(low, ymax + 0.18 * span)

    fig.suptitle(title, fontsize=12.5, fontweight="bold", y=0.98)
    add_top_legend(fig, ax, y_anchor=0.92, ncol=2, fontsize=9.0)
    fig.tight_layout(rect=[0.0, 0.0, 1.0, 0.82])
    fig.savefig(out_path, dpi=180)
    plt.close(fig)


def split_getter(metric_key: str, split: str):
    return lambda d, h, c: d[(h, split, c)][metric_key]


def mean_getter(metric_key: str, splits: list[str]):
    return lambda d, h, c: statistics.mean(d[(h, s, c)][metric_key] for s in splits)


def generate_vs_h_plots(data, plots_dir: Path):
    panels_5_plus_mean = lambda key, mean_splits, mean_label: [
        *[(f"Split {s} (del%-qry%)", split_getter(key, s)) for s in ALL_SPLITS],
        (mean_label, mean_getter(key, mean_splits)),
    ]
    qry_panels = lambda key: [
        *[(f"Split {s}", split_getter(key, s)) for s in QUERY_SPLITS],
        ("Mean (0-100 .. 75-25)", mean_getter(key, QUERY_SPLITS)),
    ]
    del_panels = [
        *[(f"Split {s}", split_getter("del_kkeys", s)) for s in DELETE_SPLITS],
        ("Mean (25-75 .. 100-0)", mean_getter("del_kkeys", DELETE_SPLITS)),
    ]
    mem_tr_panels = [
        ("Pre-Workload (Initial Load)", mean_getter("tr_before", ALL_SPLITS)),
        *[(f"After Split {s}", split_getter("tr_after", s)) for s in DELETE_SPLITS],
        ("After-Delete Splits Mean", mean_getter("tr_after", DELETE_SPLITS)),
    ]

    plot_multi_panel_vs_h(
        data,
        plots_dir / "vs_h_total_throughput.png",
        "Overall Mixed Throughput vs. Tile Size h (Higher is Better)",
        "Throughput (kops/s)",
        panels_5_plus_mean("total_kops", ALL_SPLITS, "Mean Across All 5 Splits"),
        nrows=2,
        ncols=3,
        figsize=(14.5, 8.2),
    )
    plot_multi_panel_vs_h(
        data,
        plots_dir / "vs_h_query_throughput_io.png",
        "Point-Query Throughput w/ I/O vs. Tile Size h (Higher is Better)",
        "Uncached Query Throughput (kqps)",
        qry_panels("io_kqps"),
        nrows=1,
        ncols=5,
        figsize=(17.5, 4.6),
    )
    plot_multi_panel_vs_h(
        data,
        plots_dir / "vs_h_query_throughput_mem.png",
        "Point-Query Throughput w/o I/O (In-Memory) vs. Tile Size h (Higher is Better)",
        "In-Memory Query Throughput (kqps)",
        qry_panels("mem_kqps"),
        nrows=1,
        ncols=5,
        figsize=(17.5, 4.6),
    )
    plot_multi_panel_vs_h(
        data,
        plots_dir / "vs_h_delete_throughput.png",
        "Secondary Range-Delete Throughput vs. Tile Size h (Higher is Better)",
        "Deleted Keys / s (k keys/s)",
        del_panels,
        nrows=1,
        ncols=5,
        figsize=(17.5, 4.6),
    )
    plot_multi_panel_vs_h(
        data,
        plots_dir / "vs_h_query_latency_io.png",
        "Point-Query Latency w/ I/O vs. Tile Size h (Lower is Better)",
        "Uncached Query Latency (us/op)",
        qry_panels("io_us"),
        nrows=1,
        ncols=5,
        figsize=(17.5, 4.6),
        zero_floor=False,
    )
    plot_multi_panel_vs_h(
        data,
        plots_dir / "vs_h_query_latency_mem.png",
        "Point-Query Latency w/o I/O (In-Memory) vs. Tile Size h (Lower is Better)",
        "In-Memory Query Latency (ns/op)",
        qry_panels("mem_ns"),
        nrows=1,
        ncols=5,
        figsize=(17.5, 4.6),
        zero_floor=False,
    )
    plot_multi_panel_vs_h(
        data,
        plots_dir / "vs_h_write_amplification.png",
        "Write Amplification vs. Tile Size h (Lower is Better)",
        "Write Amplification (x)",
        panels_5_plus_mean("w_amp", DELETE_SPLITS, "Mean Across Delete Splits"),
        nrows=2,
        ncols=3,
        figsize=(14.5, 8.2),
        zero_floor=False,
    )
    plot_multi_panel_vs_h(
        data,
        plots_dir / "vs_h_space_amplification.png",
        "Post-Workload Space Amplification vs. Tile Size h (Lower is Better)",
        "Space Amplification (live / compacted)",
        panels_5_plus_mean("sp_amp", DELETE_SPLITS, "Mean Across Delete Splits"),
        nrows=2,
        ncols=3,
        figsize=(14.5, 8.2),
        zero_floor=False,
    )
    plot_multi_panel_vs_h(
        data,
        plots_dir / "vs_h_memory_table_reader.png",
        "Pinned TableReader Memory Consumption vs. Tile Size h (Lower is Better)",
        "TableReader Pinned RAM (KB)",
        mem_tr_panels,
        nrows=2,
        ncols=3,
        figsize=(14.5, 8.2),
    )
    plot_single_panel_vs_h(
        data,
        plots_dir / "vs_h_memory_filter_bpk.png",
        "Filter Space (Bits per Key) vs. Tile Size h (Lower is Better)",
        "Filter Bits / Key (flt_bpk)",
        mean_getter("flt_bpk", ALL_SPLITS),
    )
    plot_single_panel_vs_h(
        data,
        plots_dir / "vs_h_memory_directory_bpk.png",
        "KiWi Directory Space (Bits per Key) vs. Tile Size h (Lower is Better)",
        "Directory Bits / Key (dir_bpk)",
        mean_getter("dir_bpk", ALL_SPLITS),
    )


def generate_vs_split_plots(data, plots_dir: Path, fixed_h: int):
    prefix = f"vs_split_h{fixed_h}"
    specs = [
        (
            f"{prefix}_total_throughput.png",
            ALL_SPLITS,
            "total_kops",
            f"Overall Mixed Throughput vs. Delete-Query Split (h = {fixed_h}, Higher is Better)",
            "Throughput (kops/s)",
            True,
        ),
        (
            f"{prefix}_query_throughput_io.png",
            QUERY_SPLITS,
            "io_kqps",
            f"Point-Query Throughput w/ I/O vs. Split (h = {fixed_h}, Higher is Better)",
            "Uncached Query Throughput (kqps)",
            False,
        ),
        (
            f"{prefix}_query_throughput_mem.png",
            QUERY_SPLITS,
            "mem_kqps",
            f"Point-Query Throughput w/o I/O (In-Memory) vs. Split (h = {fixed_h}, Higher is Better)",
            "In-Memory Query Throughput (kqps)",
            False,
        ),
        (
            f"{prefix}_delete_throughput.png",
            DELETE_SPLITS,
            "del_kkeys",
            f"Secondary Range-Delete Throughput vs. Split (h = {fixed_h}, Higher is Better)",
            "Deleted Keys / s (k keys/s)",
            True,
        ),
        (
            f"{prefix}_query_latency_io.png",
            QUERY_SPLITS,
            "io_us",
            f"Point-Query Latency w/ I/O vs. Split (h = {fixed_h}, Lower is Better)",
            "Uncached Query Latency (us/op)",
            False,
        ),
        (
            f"{prefix}_query_latency_mem.png",
            QUERY_SPLITS,
            "mem_ns",
            f"Point-Query Latency w/o I/O (In-Memory) vs. Split (h = {fixed_h}, Lower is Better)",
            "In-Memory Query Latency (ns/op)",
            False,
        ),
        (
            f"{prefix}_write_amplification.png",
            ALL_SPLITS,
            "w_amp",
            f"Write Amplification vs. Delete-Query Split (h = {fixed_h}, Lower is Better)",
            "Write Amplification (x)",
            False,
        ),
        (
            f"{prefix}_space_amplification.png",
            ALL_SPLITS,
            "sp_amp",
            f"Post-Workload Space Amplification vs. Split (h = {fixed_h}, Lower is Better)",
            "Space Amplification (live / compacted)",
            False,
        ),
        (
            f"{prefix}_memory_table_reader.png",
            ALL_SPLITS,
            "tr_after",
            f"Post-Workload Pinned TableReader RAM vs. Split (h = {fixed_h}, Lower is Better)",
            "TableReader Pinned RAM After Workload (KB)",
            True,
        ),
        (
            f"{prefix}_memory_sst_size.png",
            ALL_SPLITS,
            "sst_after",
            f"Post-Workload Live SST Footprint vs. Split (h = {fixed_h}, Lower is Better)",
            "Live SST Size After Workload (MB)",
            True,
        ),
    ]
    for fname, splits, key, title, ylabel, zf in specs:
        plot_single_panel_vs_split(
            data, plots_dir / fname, fixed_h, splits, key, title, ylabel, zero_floor=zf
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--fixed-h",
        type=int,
        default=8,
        choices=TILE_SIZES,
        help="Tile size h for single-h vs-split plots (default: 8).",
    )
    args = parser.parse_args()

    kiwi_dir = Path(__file__).resolve().parent
    md_path = kiwi_dir / "tile_filter.md"
    plots_dir = kiwi_dir / "plots"
    plots_dir.mkdir(parents=True, exist_ok=True)

    # Remove stale PNGs before regenerating cleanly.
    for old_png in plots_dir.glob("*.png"):
        old_png.unlink()

    data = parse_benchmark_markdown(md_path)
    expected_rows = len(TILE_SIZES) * len(ALL_SPLITS) * len(CANDIDATES)
    if len(data) != expected_rows:
        raise RuntimeError(f"Expected {expected_rows} table rows, parsed {len(data)}")

    generate_vs_h_plots(data, plots_dir)
    generate_vs_split_plots(data, plots_dir, args.fixed_h)

    generated = sorted(p.name for p in plots_dir.glob("*.png"))
    print(f"Generated {len(generated)} plots in {plots_dir}:")
    for name in generated:
        print(f"  - {name}")


if __name__ == "__main__":
    main()
