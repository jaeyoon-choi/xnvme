#!/usr/bin/env python3
"""
Plot uPCIe IOMMU overhead comparisons
=====================================
"""
import errno
import json
import logging as log
import os
import traceback
from argparse import ArgumentParser
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt

COMPARISON_JSON = "upcie-iommu-overhead-comparison.json"
TAIL_LATENCIES = [
    ("p99_9", "P99.9"),
    ("p99_99", "P99.99"),
    ("p99_999", "P99.999"),
]


def add_args(parser: ArgumentParser):
    parser.add_argument("--path", type=Path, default=None)


def safe_name(value):
    return str(value).replace("/", "-")


def grouped_items(items):
    grouped = defaultdict(list)
    for item in items:
        ctx = item["ctx"]
        grouped[(ctx["rw"], ctx["iosize"])].append(item)
    for entries in grouped.values():
        entries.sort(key=lambda item: item["ctx"]["iodepth"])
    return grouped


def load_comparison(search):
    path = Path(search) / "artifacts" / COMPARISON_JSON
    if not path.exists():
        path = Path(search) / COMPARISON_JSON
    with path.open() as jfd:
        return json.load(jfd)


def plot_iops(entries, output_path):
    plot_dual_series(
        entries,
        output_path,
        y_key="iops",
        ylabel="IOPS",
        legend_uio="uio_pci_generic (IOMMU off)",
        legend_vfio="vfio-pci (IOMMU on)",
    )


def plot_fio_iops(entries, output_path):
    plot_dual_series(
        entries,
        output_path,
        y_key="fio_iops",
        ylabel="FIO IOPS",
        legend_uio="uio_pci_generic (IOMMU off)",
        legend_vfio="vfio-pci (IOMMU on)",
    )


def plot_latency(entries, output_path):
    plot_dual_series(
        entries,
        output_path,
        y_key="lat_ns",
        ylabel="FIO mean latency (us)",
        legend_uio="uio_pci_generic (IOMMU off)",
        legend_vfio="vfio-pci (IOMMU on)",
        scale=1000.0,
    )


def plot_tail_latency(entries, output_path, percentile, label):
    qdepths = [item["ctx"]["iodepth"] for item in entries]
    uio = [item["uio"]["tail_lat_ns"][percentile] / 1000.0 for item in entries]
    vfio = [item["vfio"]["tail_lat_ns"][percentile] / 1000.0 for item in entries]
    xs = range(len(qdepths))
    width = 0.35

    plt.clf()
    fig, ax = plt.subplots()
    ax.bar(
        [x - width / 2 for x in xs],
        uio,
        width,
        label="uio_pci_generic (IOMMU off)",
    )
    ax.bar(
        [x + width / 2 for x in xs],
        vfio,
        width,
        label="vfio-pci (IOMMU on)",
    )
    ax.set_xlabel("iodepth")
    ax.set_ylabel(f"FIO {label} completion latency (us)")
    ax.set_xticks(list(xs), [str(qd) for qd in qdepths])
    ax.legend()
    fig.tight_layout()
    plt.savefig(output_path)


def plot_dual_series(
    entries, output_path, y_key, ylabel, legend_uio, legend_vfio, scale=1.0
):
    qdepths = [item["ctx"]["iodepth"] for item in entries]
    uio = [item["uio"][y_key] / scale for item in entries]
    vfio = [item["vfio"][y_key] / scale for item in entries]
    xs = range(len(qdepths))
    width = 0.35

    plt.clf()
    fig, ax = plt.subplots()
    ax.bar(
        [x - width / 2 for x in xs],
        uio,
        width,
        label=legend_uio,
    )
    ax.bar(
        [x + width / 2 for x in xs],
        vfio,
        width,
        label=legend_vfio,
    )
    ax.set_xlabel("iodepth")
    ax.set_ylabel(ylabel)
    ax.set_xticks(list(xs), [str(qd) for qd in qdepths])
    ax.legend()
    fig.tight_layout()
    plt.savefig(output_path)


def _is_multi_items(items):
    return bool(items) and "devcount" in items[0].get("ctx", {})


def _aggregate_multi(items):
    """Group multi-device items by (rw, iosize, devcount, iodepth) and sum
    IOPS / mean latency across devices. Returns a list with the same shape
    as single-device entries (one record per workload point) so it can be
    fed straight into the existing plot_* helpers via plot_iops/latency.
    """
    by_iod = defaultdict(list)
    for it in items:
        c = it["ctx"]
        by_iod[(c["rw"], c["iosize"], int(c["devcount"]), int(c["iodepth"]))].append(it)

    grouped = defaultdict(list)
    for (rw, iosize, devcount, iod), per_dev in by_iod.items():
        u_iops = sum(it["uio"]["iops"] for it in per_dev)
        v_iops = sum(it["vfio"]["iops"] for it in per_dev)
        u_lat = sum(it["uio"]["lat_ns"] for it in per_dev) / len(per_dev)
        v_lat = sum(it["vfio"]["lat_ns"] for it in per_dev) / len(per_dev)
        u_tail = {p: sum(it["uio"]["tail_lat_ns"][p] for it in per_dev) / len(per_dev)
                  for p, _ in TAIL_LATENCIES}
        v_tail = {p: sum(it["vfio"]["tail_lat_ns"][p] for it in per_dev) / len(per_dev)
                  for p, _ in TAIL_LATENCIES}
        agg = {
            "ctx": {"rw": rw, "iosize": iosize, "iodepth": iod, "devcount": devcount},
            "uio": {"iops": u_iops, "lat_ns": u_lat, "tail_lat_ns": u_tail},
            "vfio": {"iops": v_iops, "lat_ns": v_lat, "tail_lat_ns": v_tail},
        }
        # Forward xnvmeperf cross-check for N=1 (a single per-device entry).
        if devcount == 1 and per_dev[0]["ctx"].get("has_xnvmeperf"):
            agg["uio"]["xnvmeperf_iops"] = per_dev[0]["uio"]["xnvmeperf_iops"]
            agg["vfio"]["xnvmeperf_iops"] = per_dev[0]["vfio"]["xnvmeperf_iops"]
            agg["ctx"]["has_xnvmeperf"] = True
        grouped[(rw, iosize, devcount)].append(agg)

    for entries in grouped.values():
        entries.sort(key=lambda it: it["ctx"]["iodepth"])
    return grouped


def plot_xnvmeperf_iops(entries, output_path):
    plot_dual_series(
        entries, output_path, y_key="xnvmeperf_iops",
        ylabel="xnvmeperf IOPS (cross-check)",
        legend_uio="uio_pci_generic (IOMMU off)",
        legend_vfio="vfio-pci (IOMMU on)",
    )


def _create_plots_multi(args, comparison):
    artifacts = args.output / "artifacts"
    os.makedirs(artifacts, exist_ok=True)
    for path in artifacts.glob(
        "upcie_iommu_overhead_RW=*_IOSIZE=*_DEVCOUNT=*_TYPE=*.png"
    ):
        path.unlink()

    for (rw, iosize, devcount), entries in _aggregate_multi(
        comparison["items"]
    ).items():
        prefix = (
            f"upcie_iommu_overhead_RW={safe_name(rw)}_IOSIZE={iosize}"
            f"_DEVCOUNT={devcount}"
        )
        plot_iops(entries, artifacts / f"{prefix}_TYPE=iops.png")
        plot_latency(entries, artifacts / f"{prefix}_TYPE=latency.png")
        for percentile, label in TAIL_LATENCIES:
            plot_tail_latency(
                entries,
                artifacts / f"{prefix}_TYPE={percentile}_latency.png",
                percentile, label,
            )
        if devcount == 1 and entries and entries[0]["ctx"].get("has_xnvmeperf"):
            plot_xnvmeperf_iops(
                entries, artifacts / f"{prefix}_TYPE=xnvmeperf_iops.png"
            )
    return 0


def create_plots(args):
    search = args.path or args.output
    if not search:
        return errno.EINVAL

    comparison = load_comparison(search)
    if _is_multi_items(comparison["items"]):
        return _create_plots_multi(args, comparison)

    artifacts = args.output / "artifacts"
    os.makedirs(artifacts, exist_ok=True)
    for path in artifacts.glob("upcie_iommu_overhead_RW=*_IOSIZE=*_TYPE=*.png"):
        path.unlink()

    for (rw, iosize), entries in grouped_items(comparison["items"]).items():
        prefix = f"upcie_iommu_overhead_RW={safe_name(rw)}_IOSIZE={iosize}"
        plot_iops(entries, artifacts / f"{prefix}_TYPE=iops.png")
        plot_fio_iops(entries, artifacts / f"{prefix}_TYPE=fio_iops.png")
        plot_latency(entries, artifacts / f"{prefix}_TYPE=latency.png")
        for percentile, label in TAIL_LATENCIES:
            plot_tail_latency(
                entries,
                artifacts / f"{prefix}_TYPE={percentile}_latency.png",
                percentile,
                label,
            )

    return 0


def main(args, cijoe):
    try:
        return create_plots(args)
    except Exception as exc:
        log.error(f"Something failed({exc})")
        log.error("".join(traceback.format_exception(None, exc, exc.__traceback__)))
        return 1
