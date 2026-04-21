#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)

XNVME_DRIVER="${ROOT_DIR}/toolbox/xnvme-driver.sh"
XNVME_BIN="${ROOT_DIR}/builddir/tools/xnvme"
XNVMEPERF_BIN="${ROOT_DIR}/builddir/tools/xnvmeperf"
FIO_BIN="fio"

BDF=""
RUNNER=""
NSID="1"
RUNTIME=""
RAMP_TIME="5"
REPEAT="3"
PROFILE="full"
SIZE="100%"
HUGEMEM=""
RESET_ON_EXIT=1
CPUMASK="0x1"
WORKLOAD_PAUSE="5"
DRIVERS_CSV="uio_pci_generic,vfio-pci"
OUTDIR=""
NO_HUGEPAGE=0

SUMMARY_CSV=""
SUMMARY_RUNS_CSV=""
LAST_INFO_LOG=""
CASES=()
DRIVERS=()

usage() {
	cat <<'EOF'
Usage: bench-upcie-perf.sh --runner <xnvmeperf|fio> --bdf <0000:03:00.0> [options]

Compare xNVMe upcie performance with the same device bound to
`uio_pci_generic` and `vfio-pci`.

The script:
  1. Rebinds only the selected PCI function via PCI_WHITELIST
  2. Verifies that `xnvme info --be upcie` opens the device
  3. Runs either an xnvmeperf or fio matrix
  4. Stores raw output, a per-run CSV, and an averaged summary CSV under --outdir

Important:
  * `full` and `smoke` profiles include write workloads
  * use a scratch namespace / device
  * `uio_pci_generic` typically expects a no-IOMMU boot
  * fio with the xNVMe ioengine must run with `--thread=1`

Common options:
  --runner NAME          xnvmeperf | fio
  --bdf BDF              PCI BDF for the target NVMe device
  --nsid NSID            Namespace id for the device handle (default: 1)
  --runtime SEC          Seconds per benchmark case (default: 3)
  --repeat NUM           Repetitions per case and driver (default: 3)
  --workload-pause SEC   Sleep between workload cases (default: 5)
  --profile NAME         smoke | full (default: full)
  --drivers CSV          Drivers to benchmark (default: uio_pci_generic,vfio-pci)
  --outdir PATH          Output directory
  --huge-mem MB          Pass HUGEMEM=MB to xnvme-driver (default: 2048)
  --no-hugepage         Use page-backed memfd for upcie hostmem
  --xnvme PATH           xnvme binary (default: builddir/tools/xnvme)
  --driver-script PATH   xnvme-driver helper (default: toolbox/xnvme-driver.sh)
  --keep-binding         Do not call xnvme-driver reset on exit

Runner-specific:
  xnvmeperf:
    --xnvmeperf PATH     xnvmeperf binary (default: builddir/tools/xnvmeperf)
    --cpumask MASK       CPU mask for benchmark threads / affinity (default: 0x1)
    default runtime: 3

  fio:
    --fio PATH           fio binary (default: fio)
    --ramp-time SEC      fio ramp time in seconds (default: 5)
    --size VALUE         fio size parameter (default: 100%)
    cpumask popcount      fio numjobs is derived from --cpumask
    default runtime: 3

EOF
}

log() {
	printf '[%s] %s\n' "$(date '+%F %T')" "$*"
}

die() {
	printf 'ERROR: %s\n' "$*" >&2
	exit 1
}

run_root() {
	if [[ "${EUID}" -eq 0 ]]; then
		"$@"
	else
		command -v sudo >/dev/null 2>&1 || die "sudo is required when not running as root"
		sudo "$@"
	fi
}

run_upcie_root() {
	local -a env_args=()

	if [[ "$NO_HUGEPAGE" -eq 1 ]]; then
		env_args+=("HOSTMEM_NO_HUGEPAGE=1")
	fi

	if [[ "${#env_args[@]}" -gt 0 ]]; then
		run_root env "${env_args[@]}" "$@"
	else
		run_root "$@"
	fi
}

require_readable() {
	[[ -r "$1" ]] || die "Missing readable file: $1"
}

require_executable() {
	[[ -x "$1" ]] || die "Missing executable: $1"
}

require_command() {
	command -v "$1" >/dev/null 2>&1 || die "Missing command: $1"
}

validate_number() {
	[[ "$2" =~ ^[1-9][0-9]*$ ]] || die "$1 must be a positive integer"
}

validate_nonnegative_number() {
	[[ "$2" =~ ^[0-9]+$ ]] || die "$1 must be a non-negative integer"
}

cpumask_to_cpu_list() {
	local cpumask="${1,,}"
	local mask
	local bit=0
	local IFS=,
	local -a cpus=()

	[[ "$cpumask" =~ ^0x[0-9a-f]+$ ]] || die "--cpumask must be a non-zero hex value"
	mask=$((cpumask))
	((mask > 0)) || die "--cpumask must be a non-zero hex value"

	while ((mask > 0)); do
		if ((mask & 1)); then
			cpus+=("$bit")
		fi
		mask=$((mask >> 1))
		bit=$((bit + 1))
	done

	printf '%s\n' "${cpus[*]}"
}

cpumask_to_cpu_count() {
	local cpu_list
	local IFS=,
	local -a cpus=()

	cpu_list=$(cpumask_to_cpu_list "$1")
	read -r -a cpus <<< "$cpu_list"
	printf '%s\n' "${#cpus[@]}"
}

set_runner_defaults() {
	case "$RUNNER" in
	xnvmeperf)
		: "${RUNTIME:=3}"
		;;
	fio)
		: "${RUNTIME:=3}"
		;;
	*)
		die "Unsupported runner: $RUNNER"
		;;
	esac

	if [[ -z "$OUTDIR" ]]; then
		OUTDIR="${ROOT_DIR}/bench-results/upcie-perf-${RUNNER}-$(date +%Y%m%d-%H%M%S)"
	fi
}

split_csv() {
	local csv="$1"
	local item

	IFS=',' read -r -a DRIVERS <<< "$csv"
	for item in "${DRIVERS[@]}"; do
		[[ -n "$item" ]] || die "Empty entry in --drivers"
	done
}

build_cases() {
	case "$PROFILE" in
	smoke)
		cat <<'EOF'
randread 4096 1
randread 4096 64
randwrite 4096 1
read 131072 16
write 131072 16
EOF
		;;
	full)
		cat <<'EOF'
randread 4096 1
randread 4096 2
randread 4096 4
randread 4096 8
randread 4096 16
randread 4096 32
randread 4096 64
randwrite 4096 1
randwrite 4096 2
randwrite 4096 4
randwrite 4096 8
randwrite 4096 16
randwrite 4096 32
randwrite 4096 64
read 131072 1
read 131072 2
read 131072 4
read 131072 8
read 131072 16
read 131072 32
read 131072 64
write 131072 1
write 131072 2
write 131072 4
write 131072 8
write 131072 16
write 131072 32
write 131072 64
EOF
		;;
	*)
		die "Unsupported profile: $PROFILE"
		;;
	esac
}

case_supported() {
	local pattern="$1"
	local iosize="$2"
	local qdepth="$3"

	if [[ "$NO_HUGEPAGE" -eq 1 && "$qdepth" -ge 64 ]]; then
		return 1
	fi

	return 0
}

parse_xnvmeperf_output() {
	local logfile="$1"

	awk '
		/xnvmeperf \(elapsed: / {
			line = $0
			sub(/^.*xnvmeperf \(elapsed: /, "", line)
			sub(/s\).*$/, "", line)
			elapsed = line
		}
		/^[[:space:]]*Total:/ {
			iops = $2
			mibps = $3
			failed = $4
		}
		END {
			if (elapsed == "" || iops == "" || mibps == "" || failed == "") {
				exit 1
			}
			printf "%s,%s,%s,%s\n", elapsed, iops, mibps, failed
		}
	' "$logfile"
}

parse_fio_json() {
	local json_file="$1"
	local pattern="$2"

	python3 - "$json_file" "$pattern" <<'PY'
import json
import sys

path, pattern = sys.argv[1], sys.argv[2]
kind = "read" if "read" in pattern else "write"

with open(path, "r", encoding="utf-8") as fh:
    data = json.load(fh)

if not data.get("jobs"):
    raise SystemExit(1)

job = data["jobs"][0]
section = job.get(kind, {})
if not section:
    raise SystemExit(1)

iops = section.get("iops", 0.0)
bw_bytes = section.get("bw_bytes", 0)
bw_mib = bw_bytes / (1024.0 * 1024.0)
clat_ns = section.get("clat_ns", {})
lat_ns = section.get("lat_ns", {})
clat_mean = clat_ns.get("mean", lat_ns.get("mean", 0.0))
percentile = clat_ns.get("percentile", lat_ns.get("percentile", {}))
p99 = percentile.get("99.000000", percentile.get("99.00", 0.0))
runtime_ms = job.get("job_runtime", 0)
errors = job.get("error", 0)

print(f"{runtime_ms},{iops},{bw_mib},{clat_mean},{p99},{errors}")
PY
}

extract_fio_json() {
	local log_file="$1"
	local json_file="$2"

	python3 - "$log_file" "$json_file" <<'PY'
import json
import sys
from pathlib import Path

log_path = Path(sys.argv[1])
json_path = Path(sys.argv[2])
text = log_path.read_text(encoding="utf-8", errors="replace")
start = text.find("{")
if start < 0:
    raise SystemExit(1)

decoder = json.JSONDecoder()
obj, _ = decoder.raw_decode(text[start:])
json_path.write_text(json.dumps(obj, indent=2), encoding="utf-8")
PY
}

reset_driver() {
	log "Resetting ${BDF} back to the kernel NVMe driver"
	run_root env "PCI_WHITELIST=${BDF}" bash "${XNVME_DRIVER}" reset
}

configure_driver() {
	local driver="$1"
	local -a env_args

	env_args=("PCI_WHITELIST=${BDF}" "DRIVER_OVERRIDE=${driver}")
	if [[ "$NO_HUGEPAGE" -eq 1 ]]; then
		env_args+=("SKIP_HUGEPAGES=1")
	elif [[ -n "$HUGEMEM" ]]; then
		env_args+=("HUGEMEM=${HUGEMEM}")
	fi

	log "Binding ${BDF} to ${driver}"
	run_root env "${env_args[@]}" bash "${XNVME_DRIVER}"
}

capture_device_info() {
	local driver="$1"
	local driver_dir="$2"
	local info_log="${driver_dir}/device-info.txt"

	log "Verifying ${BDF} opens with be=upcie on ${driver}"
	run_upcie_root "${XNVME_BIN}" info --be upcie --dev-nsid "${NSID}" "${BDF}" 2>&1 | tee "$info_log"
	LAST_INFO_LOG="$info_log"
}

run_case_xnvmeperf() {
	local driver="$1"
	local driver_dir="$2"
	local info_log="$3"
	local pattern="$4"
	local iosize="$5"
	local qdepth="$6"
	local iteration="$7"
	local raw_log
	local metrics
	local elapsed
	local iops
	local mibps
	local failed
	local cpu_spec="$CPUMASK"

	raw_log="${driver_dir}/${pattern}_bs${iosize}_qd${qdepth}_run$(printf '%02d' "${iteration}").log"

	log "Running ${driver}: pattern=${pattern}, iosize=${iosize}, qdepth=${qdepth}, run=${iteration}/${REPEAT}"
	run_upcie_root "${XNVMEPERF_BIN}" run \
		--be upcie \
		--iopattern "${pattern}" \
		--qdepth "${qdepth}" \
		--iosize "${iosize}" \
		--runtime "${RUNTIME}" \
		--cpumask "${CPUMASK}" \
		"${BDF}" 2>&1 | tee "$raw_log"

	metrics=$(parse_xnvmeperf_output "$raw_log") || die "Failed to parse xnvmeperf output in ${raw_log}"
	IFS=, read -r elapsed iops mibps failed <<< "$metrics"

	printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
		"$RUNNER" "$driver" "$cpu_spec" "$pattern" "$iosize" "$qdepth" "$iteration" "$elapsed" "$iops" \
		"$mibps" "$failed" "$raw_log" "$info_log" >> "$SUMMARY_RUNS_CSV"
}

run_case_fio() {
	local driver="$1"
	local driver_dir="$2"
	local info_log="$3"
	local pattern="$4"
	local iosize="$5"
	local qdepth="$6"
	local iteration="$7"
	local name="${driver}_${pattern}_bs${iosize}_qd${qdepth}_run$(printf '%02d' "${iteration}")"
	local json_file="${driver_dir}/${name}.json"
	local log_file="${driver_dir}/${name}.log"
	local -a fio_args
	local metrics
	local runtime_ms
	local iops
	local bw_mib
	local clat_mean_ns
	local clat_p99_ns
	local error_code
	local cpu_spec="$CPUMASK"
	local fio_cpu_list
	local fio_numjobs

	fio_args=(
		"${FIO_BIN}"
		"--name=${name}"
		"--filename=${BDF//:/\\:}"
		"--ioengine=xnvme"
		"--xnvme_be=upcie"
		"--xnvme_dev_nsid=${NSID}"
		# Keep fio in thread mode for the xNVMe ioengine. Actual multi-core
		# execution comes from numjobs, which we derive from the cpumask.
		"--thread=1"
		"--direct=1"
		"--rw=${pattern}"
		"--size=${SIZE}"
		"--bs=${iosize}"
		"--iodepth=${qdepth}"
		"--time_based=1"
		"--runtime=${RUNTIME}"
		"--ramp_time=${RAMP_TIME}"
		"--norandommap=1"
		"--group_reporting=1"
		"--output-format=normal,json"
	)

	fio_cpu_list=$(cpumask_to_cpu_list "$CPUMASK")
	fio_numjobs=$(cpumask_to_cpu_count "$CPUMASK")
	fio_args+=(
		"--numjobs=${fio_numjobs}"
		"--cpus_allowed=${fio_cpu_list}"
		"--cpus_allowed_policy=split"
	)

	log "Running ${driver}: pattern=${pattern}, iosize=${iosize}, qdepth=${qdepth}, run=${iteration}/${REPEAT}"
	run_upcie_root "${fio_args[@]}" 2>&1 | tee "$log_file" >/dev/null
	extract_fio_json "$log_file" "$json_file" || die "Failed to extract fio JSON from ${log_file}"

	metrics=$(parse_fio_json "$json_file" "$pattern") || die "Failed to parse fio JSON in ${json_file}"
	IFS=, read -r runtime_ms iops bw_mib clat_mean_ns clat_p99_ns error_code <<< "$metrics"

	printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
		"$RUNNER" "$driver" "$cpu_spec" "$pattern" "$iosize" "$qdepth" "$iteration" "$runtime_ms" "$iops" \
		"$bw_mib" "$clat_mean_ns" "$clat_p99_ns" "$error_code" "$json_file" "$log_file" "$info_log" \
		>> "$SUMMARY_RUNS_CSV"
}

run_case() {
	case "$RUNNER" in
	xnvmeperf)
		run_case_xnvmeperf "$@"
		;;
	fio)
		run_case_fio "$@"
		;;
	esac
}

initialize_summary_runs_csv() {
	case "$RUNNER" in
	xnvmeperf)
		cat > "$SUMMARY_RUNS_CSV" <<'EOF'
runner,driver,cpu_spec,pattern,iosize,qdepth,repeat,elapsed_s,iops,mibps,failed,raw_log,info_log
EOF
		;;
	fio)
		cat > "$SUMMARY_RUNS_CSV" <<'EOF'
runner,driver,cpu_spec,pattern,iosize,qdepth,repeat,runtime_ms,iops,bw_mib,clat_mean_ns,clat_p99_ns,error,json_file,log_file,info_log
EOF
		;;
	esac
}

aggregate_summary_csv() {
	python3 - "$RUNNER" "$SUMMARY_RUNS_CSV" "$SUMMARY_CSV" <<'PY'
import csv
import re
import sys
from collections import OrderedDict

runner, runs_path, summary_path = sys.argv[1:4]

with open(runs_path, "r", encoding="utf-8", newline="") as fh:
    rows = list(csv.DictReader(fh))

if not rows:
    raise SystemExit(1)

groups = OrderedDict()
for row in rows:
    key = (
        row["runner"],
        row["driver"],
        row["cpu_spec"],
        row["pattern"],
        row["iosize"],
        row["qdepth"],
    )
    groups.setdefault(key, []).append(row)


def avg(values):
    return sum(values) / len(values)


def run_glob(path):
    return re.sub(r"_run[0-9]+(\.[^.]+)$", r"_run*\1", path)
if runner == "xnvmeperf":
    fieldnames = [
        "runner",
        "driver",
        "cpu_spec",
        "pattern",
        "iosize",
        "qdepth",
        "repeat",
        "elapsed_s",
        "iops",
        "mibps",
        "failed",
        "raw_log",
        "info_log",
    ]
else:
    fieldnames = [
        "runner",
        "driver",
        "cpu_spec",
        "pattern",
        "iosize",
        "qdepth",
        "repeat",
        "runtime_ms",
        "iops",
        "bw_mib",
        "clat_mean_ns",
        "clat_p99_ns",
        "error",
        "json_file",
        "log_file",
        "info_log",
    ]

with open(summary_path, "w", encoding="utf-8", newline="") as fh:
    writer = csv.DictWriter(fh, fieldnames=fieldnames)
    writer.writeheader()

    for rows in groups.values():
        first = rows[0]
        base = {
            "runner": first["runner"],
            "driver": first["driver"],
            "cpu_spec": first["cpu_spec"],
            "pattern": first["pattern"],
            "iosize": first["iosize"],
            "qdepth": first["qdepth"],
            "repeat": str(len(rows)),
        }

        if runner == "xnvmeperf":
            base.update(
                {
                    "elapsed_s": f"{avg([float(r['elapsed_s']) for r in rows]):.6f}",
                    "iops": f"{avg([float(r['iops']) for r in rows]):.6f}",
                    "mibps": f"{avg([float(r['mibps']) for r in rows]):.6f}",
                    "failed": str(sum(int(float(r["failed"])) for r in rows)),
                    "raw_log": run_glob(first["raw_log"]),
                    "info_log": first["info_log"],
                }
            )
        else:
            base.update(
                {
                    "runtime_ms": f"{avg([float(r['runtime_ms']) for r in rows]):.6f}",
                    "iops": f"{avg([float(r['iops']) for r in rows]):.6f}",
                    "bw_mib": f"{avg([float(r['bw_mib']) for r in rows]):.6f}",
                    "clat_mean_ns": f"{avg([float(r['clat_mean_ns']) for r in rows]):.6f}",
                    "clat_p99_ns": f"{avg([float(r['clat_p99_ns']) for r in rows]):.6f}",
                    "error": str(max(int(float(r["error"])) for r in rows)),
                    "json_file": run_glob(first["json_file"]),
                    "log_file": run_glob(first["log_file"]),
                    "info_log": first["info_log"],
                }
            )

        writer.writerow(base)
PY
}

write_metadata() {
	local meta_file="$OUTDIR/run-meta.txt"
	local git_rev="unknown"

	if command -v git >/dev/null 2>&1; then
		git_rev=$(git -C "$ROOT_DIR" rev-parse --short HEAD 2>/dev/null || printf 'unknown')
	fi

	{
		printf 'date=%s\n' "$(date --iso-8601=seconds 2>/dev/null || date)"
		printf 'git_rev=%s\n' "$git_rev"
		printf 'runner=%s\n' "$RUNNER"
		printf 'bdf=%s\n' "$BDF"
		printf 'nsid=%s\n' "$NSID"
		printf 'runtime=%s\n' "$RUNTIME"
		printf 'repeat=%s\n' "$REPEAT"
		printf 'workload_pause=%s\n' "$WORKLOAD_PAUSE"
		printf 'profile=%s\n' "$PROFILE"
		printf 'drivers=%s\n' "$DRIVERS_CSV"
		printf 'huge_mem_mb=%s\n' "${HUGEMEM:-default}"
		printf 'no_hugepage=%s\n' "$NO_HUGEPAGE"
		printf 'xnvme=%s\n' "$XNVME_BIN"
		printf 'driver_script=%s\n' "$XNVME_DRIVER"
		case "$RUNNER" in
		xnvmeperf)
			printf 'cpumask=%s\n' "$CPUMASK"
			printf 'xnvmeperf=%s\n' "$XNVMEPERF_BIN"
			;;
		fio)
			printf 'ramp_time=%s\n' "$RAMP_TIME"
			printf 'size=%s\n' "$SIZE"
			printf 'cpumask=%s\n' "$CPUMASK"
			printf 'fio_numjobs=%s\n' "$(cpumask_to_cpu_count "$CPUMASK")"
			printf 'fio_cpus_allowed=%s\n' "$(cpumask_to_cpu_list "$CPUMASK")"
			printf 'fio=%s\n' "$FIO_BIN"
			;;
		esac
	} > "$meta_file"
}

print_csv_table() {
	local csv_file="$1"

	if command -v column >/dev/null 2>&1; then
		column -s, -t "$csv_file"
	else
		cat "$csv_file"
	fi
}

print_result_tables() {
	local display_csv
	local fields

	display_csv=$(mktemp "${OUTDIR}/summary-display.XXXXXX.csv")

	case "$RUNNER" in
	xnvmeperf)
		fields="1-11"
		;;
	fio)
		fields="1-13"
		;;
	esac

	cut -d, -f"${fields}" "$SUMMARY_CSV" > "$display_csv"

	printf '\nSummary Table\n'
	print_csv_table "$display_csv"

	rm -f "$display_csv"
}

cleanup() {
	if [[ "$RESET_ON_EXIT" -eq 1 ]]; then
		reset_driver || true
	fi
}

while [[ $# -gt 0 ]]; do
	case "$1" in
	--runner)
		RUNNER="${2:-}"
		shift 2
		;;
	--bdf)
		BDF="${2:-}"
		shift 2
		;;
	--nsid)
		NSID="${2:-}"
		shift 2
		;;
	--runtime)
		RUNTIME="${2:-}"
		shift 2
		;;
	--ramp-time)
		RAMP_TIME="${2:-}"
		shift 2
		;;
	--repeat)
		REPEAT="${2:-}"
		shift 2
		;;
	--workload-pause)
		WORKLOAD_PAUSE="${2:-}"
		shift 2
		;;
	--profile)
		PROFILE="${2:-}"
		shift 2
		;;
	--drivers)
		DRIVERS_CSV="${2:-}"
		shift 2
		;;
	--size)
		SIZE="${2:-}"
		shift 2
		;;
	--cpumask)
		CPUMASK="${2:-}"
		shift 2
		;;
	--huge-mem)
		HUGEMEM="${2:-}"
		shift 2
		;;
	--no-hugepage)
		NO_HUGEPAGE=1
		shift
		;;
	--outdir)
		OUTDIR="${2:-}"
		shift 2
		;;
	--fio)
		FIO_BIN="${2:-}"
		shift 2
		;;
	--xnvme)
		XNVME_BIN="${2:-}"
		shift 2
		;;
	--xnvmeperf)
		XNVMEPERF_BIN="${2:-}"
		shift 2
		;;
	--driver-script)
		XNVME_DRIVER="${2:-}"
		shift 2
		;;
	--keep-binding)
		RESET_ON_EXIT=0
		shift
		;;
	--help|-h)
		usage
		exit 0
		;;
	*)
		die "Unknown argument: $1"
		;;
	esac
done

[[ -n "$RUNNER" ]] || {
	usage
	die "--runner is required"
}

[[ -n "$BDF" ]] || {
	usage
	die "--bdf is required"
}

set_runner_defaults
validate_number "--nsid" "$NSID"
validate_number "--runtime" "$RUNTIME"
validate_number "--repeat" "$REPEAT"
validate_nonnegative_number "--workload-pause" "$WORKLOAD_PAUSE"
cpumask_to_cpu_list "$CPUMASK" >/dev/null
if [[ -n "$HUGEMEM" ]]; then
	validate_number "--huge-mem" "$HUGEMEM"
fi
if [[ "$RUNNER" == "fio" ]]; then
	validate_number "--ramp-time" "$RAMP_TIME"
fi

split_csv "$DRIVERS_CSV"

require_readable "$XNVME_DRIVER"
require_executable "$XNVME_BIN"

case "$RUNNER" in
xnvmeperf)
	require_executable "$XNVMEPERF_BIN"
	;;
fio)
	require_command python3
	if [[ "$FIO_BIN" == */* ]]; then
		require_executable "$FIO_BIN"
	else
		require_command "$FIO_BIN"
	fi
	;;
esac

mkdir -p "$OUTDIR/raw"
SUMMARY_CSV="${OUTDIR}/summary.csv"
SUMMARY_RUNS_CSV="${OUTDIR}/summary-runs.csv"

initialize_summary_runs_csv
write_metadata
trap cleanup EXIT

log "Results will be written to ${OUTDIR}"
log "Runner ${RUNNER} will benchmark ${BDF} with nsid=${NSID} using be=upcie"

while read -r pattern iosize qdepth; do
	[[ -n "$pattern" ]] || continue
	if ! case_supported "$pattern" "$iosize" "$qdepth"; then
		log "Skipping ${pattern}/${iosize}/qd${qdepth} in no-hugepage mode"
		continue
	fi
	CASES+=("${pattern} ${iosize} ${qdepth}")
done < <(build_cases)

for driver in "${DRIVERS[@]}"; do
	driver_dir="${OUTDIR}/raw/${driver}"
	total_cases="${#CASES[@]}"
	case_index=0
	mkdir -p "$driver_dir"

	configure_driver "$driver"
	capture_device_info "$driver" "$driver_dir"
	info_log="$LAST_INFO_LOG"

	for case_def in "${CASES[@]}"; do
		case_index=$((case_index + 1))
		read -r pattern iosize qdepth <<< "$case_def"
		for ((iteration = 1; iteration <= REPEAT; iteration++)); do
			run_case "$driver" "$driver_dir" "$info_log" "$pattern" "$iosize" "$qdepth" "$iteration"
		done
		if ((WORKLOAD_PAUSE > 0 && case_index < total_cases)); then
			log "Sleeping ${WORKLOAD_PAUSE}s before the next workload"
			sleep "$WORKLOAD_PAUSE"
		fi
	done
done

aggregate_summary_csv

log "Benchmark complete"
log "Per-run CSV    : ${SUMMARY_RUNS_CSV}"
log "Summary CSV    : ${SUMMARY_CSV}"
print_result_tables
