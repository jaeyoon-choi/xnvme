#!/usr/bin/env python3
"""
Produce a uPCIe IOMMU overhead report
=====================================
"""
import errno
import json
import logging as log
import re
import shlex
import shutil
import traceback
from argparse import ArgumentParser
from collections import defaultdict
from pathlib import Path

import jinja2
from reporter import (
    copy_graphs,
    create_stylesheet,
    create_test_setup,
    create_xnvme_cover,
    create_xnvme_info,
)

from cijoe.core.resources import dict_from_yamlfile

COMPARISON_JSON = "upcie-iommu-overhead-comparison.json"
PLOT_PATH_REGEX = r".*_RW=(?P<rw>.+)_IOSIZE=(?P<iosize>\d+)_TYPE=(?P<type>.+)\.png"
TAIL_LATENCIES = [
    ("p99_9", "P99.9"),
    ("p99_99", "P99.99"),
    ("p99_999", "P99.999"),
]


def add_args(parser: ArgumentParser):
    parser.add_argument(
        "--templates", type=Path, default=Path.cwd() / "templates" / "perf_report"
    )
    parser.add_argument("--path", type=Path, default=None)
    parser.add_argument(
        "--runs",
        type=Path,
        default=None,
        help="Path to the workload matrix used by the compared benchmark runs",
    )
    parser.add_argument("--report_title", type=str, default="xNVMe uPCIe")
    parser.add_argument("--report_subtitle", type=str, default="IOMMU Overhead Report")


def load_comparison(search):
    path = Path(search) / "artifacts" / COMPARISON_JSON
    if not path.exists():
        path = Path(search) / COMPARISON_JSON
    with path.open() as jfd:
        return json.load(jfd)


def format_items(items):
    rows = []
    for item in items:
        row = {
            "rw": item["ctx"]["rw"],
            "iosize": item["ctx"]["iosize"],
            "iodepth": item["ctx"]["iodepth"],
            "throughput_runner": "xnvmeperf",
            "latency_runner": "fio",
            "uio_iops": f"{item['uio']['iops']:.2f}",
            "vfio_iops": f"{item['vfio']['iops']:.2f}",
            "delta_pct": f"{item['delta_pct']:.2f}",
            "uio_fio_iops": f"{item['uio']['fio_iops']:.2f}",
            "vfio_fio_iops": f"{item['vfio']['fio_iops']:.2f}",
            "fio_delta_pct": f"{item['fio_delta_pct']:.2f}",
            "uio_fio_cv": f"{item['uio']['fio_iops_cv']:.2f}",
            "vfio_fio_cv": f"{item['vfio']['fio_iops_cv']:.2f}",
            "uio_lat_us": f"{item['uio']['lat_ns'] / 1000.0:.2f}",
            "vfio_lat_us": f"{item['vfio']['lat_ns'] / 1000.0:.2f}",
            "lat_delta_pct": f"{item['lat_delta_pct']:.2f}",
            "uio_cv": f"{item['uio']['iops_cv']:.2f}",
            "vfio_cv": f"{item['vfio']['iops_cv']:.2f}",
        }
        for percentile, _ in TAIL_LATENCIES:
            row[f"uio_{percentile}_us"] = (
                f"{item['uio']['tail_lat_ns'][percentile] / 1000.0:.2f}"
            )
            row[f"vfio_{percentile}_us"] = (
                f"{item['vfio']['tail_lat_ns'][percentile] / 1000.0:.2f}"
            )
            row[f"{percentile}_delta_pct"] = (
                f"{item['tail_lat_delta_pct'][percentile]:.2f}"
            )
        rows.append(row)
    return rows


def plot_groups(artifacts):
    groups = defaultdict(dict)
    for path in artifacts.glob("upcie_iommu_overhead_RW=*_IOSIZE=*_TYPE=*.png"):
        match = re.match(PLOT_PATH_REGEX, path.name)
        if not match:
            continue
        key = (match.group("rw"), match.group("iosize"))
        groups[key][match.group("type")] = path.name
    return groups


def workload_matrix(runs_path):
    if not runs_path:
        return []

    runs = dict_from_yamlfile(runs_path)
    matrix = []
    for workload in runs.get("workloads", []):
        matrix.append(
            {
                "rw": workload["pattern"],
                "iosizes": ", ".join(str(value) for value in workload["iosizes"]),
                "iodepths": ", ".join(str(value) for value in workload["iodepths"]),
            }
        )
    return matrix


def report_sections(plots, rows):
    grouped_rows = defaultdict(list)
    for row in rows:
        grouped_rows[(row["rw"], str(row["iosize"]))].append(row)

    sections = []
    for key, group_plots in sorted(plots.items(), key=lambda item: item[0]):
        group_rows = sorted(
            grouped_rows.get(key, []),
            key=lambda row: int(row["iodepth"]),
        )
        sections.append(
            {
                "rw": key[0],
                "iosize": key[1],
                "plots": group_plots,
                "rows": group_rows,
            }
        )
    return sections


def _is_multi(comparison):
    items = comparison.get("items") or []
    return bool(items) and "devcount" in items[0].get("ctx", {})


PLOT_PATH_REGEX_MULTI = (
    r".*_RW=(?P<rw>.+)_IOSIZE=(?P<iosize>\d+)_DEVCOUNT=(?P<devcount>\d+)"
    r"_TYPE=(?P<type>.+)\.png"
)


def _plot_groups_multi(artifacts):
    groups = defaultdict(dict)
    for path in artifacts.glob(
        "upcie_iommu_overhead_RW=*_IOSIZE=*_DEVCOUNT=*_TYPE=*.png"
    ):
        m = re.match(PLOT_PATH_REGEX_MULTI, path.name)
        if not m:
            continue
        groups[(m.group("rw"), m.group("iosize"), int(m.group("devcount")))][
            m.group("type")
        ] = path.name
    return groups


def _pct(base, value):
    return (value - base) / base * 100.0 if base else float("nan")


def _multi_aggregate_rows(items):
    """One row per (rw, iosize, iodepth, devcount): IOPS sum, latency mean."""
    by_iod = defaultdict(list)
    for it in items:
        c = it["ctx"]
        by_iod[(c["rw"], int(c["iosize"]), int(c["iodepth"]), int(c["devcount"]))].append(it)
    rows = []
    for (rw, iosize, iodepth, devcount), per_dev in by_iod.items():
        n = len(per_dev)
        u_iops = sum(it["uio"]["iops"] for it in per_dev)
        v_iops = sum(it["vfio"]["iops"] for it in per_dev)
        u_lat = sum(it["uio"]["lat_ns"] for it in per_dev) / n
        v_lat = sum(it["vfio"]["lat_ns"] for it in per_dev) / n
        u_tail = {p: sum(it["uio"]["tail_lat_ns"][p] for it in per_dev) / n
                  for p, _ in TAIL_LATENCIES}
        v_tail = {p: sum(it["vfio"]["tail_lat_ns"][p] for it in per_dev) / n
                  for p, _ in TAIL_LATENCIES}
        u_cv = sum(it["uio"].get("iops_cv", 0) for it in per_dev) / n
        v_cv = sum(it["vfio"].get("iops_cv", 0) for it in per_dev) / n
        row = {
            "rw": rw, "iosize": iosize, "iodepth": iodepth, "devcount": devcount,
            "uio_iops": f"{u_iops:.2f}", "vfio_iops": f"{v_iops:.2f}",
            "iops_delta_pct": f"{_pct(u_iops, v_iops):.2f}",
            "uio_lat_us": f"{u_lat / 1000.0:.2f}",
            "vfio_lat_us": f"{v_lat / 1000.0:.2f}",
            "lat_delta_pct": f"{_pct(u_lat, v_lat):.2f}",
            "uio_iops_cv": f"{u_cv:.2f}", "vfio_iops_cv": f"{v_cv:.2f}",
            "has_xnvmeperf": False,
        }
        for p, _label in TAIL_LATENCIES:
            row[f"uio_{p}_us"] = f"{u_tail[p] / 1000.0:.2f}"
            row[f"vfio_{p}_us"] = f"{v_tail[p] / 1000.0:.2f}"
            row[f"{p}_delta_pct"] = f"{_pct(u_tail[p], v_tail[p]):.2f}"
        if devcount == 1 and per_dev[0]["ctx"].get("has_xnvmeperf"):
            u_xp = per_dev[0]["uio"]["xnvmeperf_iops"]
            v_xp = per_dev[0]["vfio"]["xnvmeperf_iops"]
            row["has_xnvmeperf"] = True
            row["uio_xnvmeperf_iops"] = f"{u_xp:.2f}"
            row["vfio_xnvmeperf_iops"] = f"{v_xp:.2f}"
            row["xnvmeperf_iops_delta_pct"] = f"{_pct(u_xp, v_xp):.2f}"
        rows.append(row)
    return rows


def _multi_per_device_rows(items):
    rows = []
    for it in items:
        c = it["ctx"]
        if int(c["devcount"]) <= 1:
            continue
        u_iops = it["uio"]["iops"]; v_iops = it["vfio"]["iops"]
        u_lat = it["uio"]["lat_ns"]; v_lat = it["vfio"]["lat_ns"]
        rows.append({
            "rw": c["rw"], "iosize": c["iosize"], "iodepth": c["iodepth"],
            "devcount": c["devcount"], "dev": c["dev"],
            "uio_iops": f"{u_iops:.2f}", "vfio_iops": f"{v_iops:.2f}",
            "iops_delta_pct": f"{_pct(u_iops, v_iops):.2f}",
            "uio_lat_us": f"{u_lat / 1000.0:.2f}",
            "vfio_lat_us": f"{v_lat / 1000.0:.2f}",
        })
    rows.sort(key=lambda r: (r["rw"], int(r["iosize"]), int(r["devcount"]),
                              int(r["iodepth"]), str(r["dev"])))
    return rows


def _multi_sections(plots, agg_rows):
    grouped = defaultdict(list)
    for row in agg_rows:
        grouped[(row["rw"], str(row["iosize"]), int(row["devcount"]))].append(row)
    sections = []
    for key, group_plots in sorted(plots.items(), key=lambda i: i[0]):
        rows = sorted(grouped.get(key, []), key=lambda r: int(r["iodepth"]))
        sections.append({
            "rw": key[0], "iosize": key[1], "devcount": key[2],
            "plots": group_plots, "rows": rows,
        })
    return sections


def _multi_per_device_groups(per_dev_rows):
    grouped = defaultdict(list)
    for row in per_dev_rows:
        grouped[(row["rw"], str(row["iosize"]), int(row["devcount"]))].append(row)
    return [
        {"rw": k[0], "iosize": k[1], "devcount": k[2], "rows": v}
        for k, v in sorted(grouped.items())
    ]


def _render_multi(args, cijoe, comparison, search_path, artifacts,
                  templates_path, report_path, style_path, cover_path):
    body_path = report_path / "report.rst"
    pdf_path = report_path / "upcie-iommu-overhead.pdf"
    items = comparison["items"]
    agg = _multi_aggregate_rows(items)
    per_dev = _multi_per_device_rows(items)
    plots = _plot_groups_multi(artifacts)
    sections = _multi_sections(plots, agg)
    workloads = workload_matrix(args.runs)

    env = jinja2.Environment(loader=jinja2.FileSystemLoader(templates_path))
    template = env.get_template("upcie_iommu_overhead_multi.jinja2.rst")
    body_path.write_text(template.render({
        "title": args.report_title,
        "subtitle": args.report_subtitle,
        "rows": agg,
        "plots": plots,
        "workloads": workloads,
        "sections": sections,
        "per_device": _multi_per_device_groups(per_dev),
        "tail_latencies": TAIL_LATENCIES,
    }))
    err, _ = cijoe.run_local(
        f"rst2pdf {shlex.quote(str(body_path))}"
        f" -b1"
        f" --custom-cover {shlex.quote(str(cover_path))}"
        f" -s {shlex.quote(str(style_path))}"
        f" -o {shlex.quote(str(pdf_path))}"
    )
    return err


def main(args, cijoe):
    try:
        templates_path = args.templates.resolve()
        search_path = Path(args.path or cijoe.output_path).resolve()
        artifacts = search_path / "artifacts"

        report_path = cijoe.output_path / "artifacts" / "perf_report"
        if report_path.exists():
            shutil.rmtree(report_path)
        report_path.mkdir(parents=False, exist_ok=True)

        body_path = report_path / "report.rst"
        pdf_path = report_path / "upcie-iommu-overhead.pdf"
        style_path = create_stylesheet(templates_path, report_path)
        cover_path = create_xnvme_cover(
            templates_path, report_path, args.report_title, args.report_subtitle
        )
        create_xnvme_info(templates_path, report_path)
        create_test_setup(templates_path, report_path, artifacts)
        copy_graphs(report_path, artifacts)

        comparison = load_comparison(search_path)
        if _is_multi(comparison):
            return _render_multi(
                args, cijoe, comparison, search_path, artifacts,
                templates_path, report_path, style_path, cover_path,
            )

        rows = format_items(comparison["items"])
        plots = plot_groups(artifacts)
        workloads = workload_matrix(args.runs)
        sections = report_sections(plots, rows)

        template_loader = jinja2.FileSystemLoader(templates_path)
        template_env = jinja2.Environment(loader=template_loader)
        template = template_env.get_template("upcie_iommu_overhead.jinja2.rst")

        with body_path.open("w") as body:
            body.write(
                template.render(
                    {
                        "title": args.report_title,
                        "subtitle": args.report_subtitle,
                        "comparison": comparison,
                        "rows": rows,
                        "plots": plots,
                        "workloads": workloads,
                        "sections": sections,
                        "tail_latencies": TAIL_LATENCIES,
                    }
                )
            )

        err, _ = cijoe.run_local(
            f"rst2pdf {shlex.quote(str(body_path))}"
            f" -b1"
            f" --custom-cover {shlex.quote(str(cover_path))}"
            f" -s {shlex.quote(str(style_path))}"
            f" -o {shlex.quote(str(pdf_path))}"
        )
        return err
    except StopIteration:
        log.error("Missing comparison artifact")
        return errno.ENOENT
    except Exception as exc:
        log.error(f"Something failed({exc})")
        log.error("".join(traceback.format_exception(None, exc, exc.__traceback__)))
        return 1
