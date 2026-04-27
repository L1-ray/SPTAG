#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  scripts/run_io_analysis.sh -c <config.ini> -d <device> -o <output_dir> [options]

Required:
  -c, --config            Path to ssdserving config
  -d, --device            Disk device name (e.g. nvme0n1, sda)
  -o, --output-dir        Output directory

Optional:
  -b, --binary            ssdserving binary path (default: ./Release/ssdserving)
  -m, --max-read-mbps     Device baseline read MB/s (default: 0)
  -q, --query-csv         Per-query CSV path from EnableDetailedIOStats
  -i, --interval-ms       Monitor sampling interval ms (default: 100)
EOF
}

BINARY="./Release/ssdserving"
CONFIG=""
DEVICE=""
OUTPUT_DIR=""
MAX_READ_MBPS="0"
QUERY_CSV=""
INTERVAL_MS="100"

while [[ $# -gt 0 ]]; do
  case "$1" in
    -b|--binary) BINARY="$2"; shift 2 ;;
    -c|--config) CONFIG="$2"; shift 2 ;;
    -d|--device) DEVICE="$2"; shift 2 ;;
    -o|--output-dir) OUTPUT_DIR="$2"; shift 2 ;;
    -m|--max-read-mbps) MAX_READ_MBPS="$2"; shift 2 ;;
    -q|--query-csv) QUERY_CSV="$2"; shift 2 ;;
    -i|--interval-ms) INTERVAL_MS="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *)
      echo "Unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ -z "$CONFIG" || -z "$DEVICE" || -z "$OUTPUT_DIR" ]]; then
  usage
  exit 1
fi

mkdir -p "$OUTPUT_DIR"

LOG_FILE="$OUTPUT_DIR/sptag.log"
REPORT_FILE="$OUTPUT_DIR/report.md"

"$BINARY" "$CONFIG" >"$LOG_FILE" 2>&1 &
TARGET_PID=$!

python3 scripts/spann_io_monitor.py \
  --device "$DEVICE" \
  --pid "$TARGET_PID" \
  --interval-ms "$INTERVAL_MS" \
  --device-max-read-mbps "$MAX_READ_MBPS" \
  --output-dir "$OUTPUT_DIR" &
MONITOR_PID=$!

wait "$TARGET_PID"
wait "$MONITOR_PID"

ANALYZE_ARGS=(
  --disk-csv "$OUTPUT_DIR/disk_stats.csv"
  --process-csv "$OUTPUT_DIR/process_io_stats.csv"
  --cpu-csv "$OUTPUT_DIR/cpu_stats.csv"
  --psi-csv "$OUTPUT_DIR/psi_io_stats.csv"
  --sptag-log "$LOG_FILE"
  --output-report "$REPORT_FILE"
)

if [[ -n "$QUERY_CSV" ]]; then
  ANALYZE_ARGS+=(--query-csv "$QUERY_CSV")
fi

python3 scripts/analyze_spann_io.py "${ANALYZE_ARGS[@]}"
echo "Analysis report: $REPORT_FILE"
