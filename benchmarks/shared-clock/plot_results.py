#!/usr/bin/env python3
from __future__ import annotations

import csv
from pathlib import Path
import statistics
import sys

import matplotlib.pyplot as plt


LABELS = {
    "native": "Native",
    "legacy": "Legacy timestamp file",
    "shared": "Shared-memory clock",
}
COLORS = {"native": "#4C78A8", "legacy": "#E45756", "shared": "#54A24B"}


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: plot_results.py RAW_RESULTS.csv OUTPUT_DIR")
    results_path = Path(sys.argv[1])
    output_dir = Path(sys.argv[2])
    values: dict[str, list[float]] = {mode: [] for mode in LABELS}
    with results_path.open(newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            values[row["mode"]].append(float(row["latency_ns"]))

    modes = list(LABELS)
    data = [values[mode] for mode in modes]
    fig, ax = plt.subplots(figsize=(8.2, 5.2), constrained_layout=True)
    box = ax.boxplot(data, patch_artist=True, widths=0.55, showfliers=False)
    for patch, mode in zip(box["boxes"], modes):
        patch.set_facecolor(COLORS[mode])
        patch.set_alpha(0.72)
    for index, mode in enumerate(modes, start=1):
        samples = values[mode]
        offsets = [((i % 5) - 2) * 0.018 for i in range(len(samples))]
        ax.scatter(
            [index + offset for offset in offsets],
            samples,
            s=24,
            color=COLORS[mode],
            edgecolor="white",
            linewidth=0.45,
            zorder=3,
        )
        median = statistics.median(samples)
        ax.annotate(
            f"median {median:.1f} ns",
            (index, max(samples)),
            xytext=(0, 8),
            textcoords="offset points",
            ha="center",
            fontsize=9,
        )

    ax.set_yscale("log")
    ax.set_xticks(range(1, len(modes) + 1), [LABELS[mode] for mode in modes])
    ax.set_ylabel("clock_gettime(CLOCK_REALTIME) latency (ns, log scale)")
    ax.set_title("libfaketime realtime-read latency")
    ax.grid(axis="y", which="both", alpha=0.25)
    for suffix in ("png", "pdf", "svg"):
        fig.savefig(output_dir / f"shared_clock_latency.{suffix}", dpi=240)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
