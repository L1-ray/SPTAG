#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  scripts/run_io_analysis.sh -c <config.ini> -o <output_dir> [options]

Required:
  -c, --config            Path to ssdserving config
  -o, --output-dir        Output directory

Optional:
  -d, --device            Disk device name (overrides auto-detect)
  -p, --data-path         Data/index path used for auto-detect device
  -b, --binary            ssdserving binary path (default: ./Release/ssdserving)
  -m, --max-read-mbps     Device baseline read MB/s (default: 0)
  -q, --query-csv         Per-query CSV path from EnableDetailedIOStats
  -i, --interval-ms       Monitor sampling interval ms (default: 100)
  -C, --clear-cache       Drop OS page cache before run (requires passwordless sudo)
EOF
}

BINARY="./Release/ssdserving"
CONFIG=""
DEVICE=""
DATA_PATH=""
OUTPUT_DIR=""
MAX_READ_MBPS="0"
QUERY_CSV=""
INTERVAL_MS="100"
CLEAR_CACHE="false"

trim_spaces() {
  local v="$1"
  v="${v#"${v%%[![:space:]]*}"}"
  v="${v%"${v##*[![:space:]]}"}"
  printf '%s' "$v"
}

extract_config_value() {
  local config_path="$1"
  local key="$2"
  local line
  line=$(awk -F= -v k="$key" '$1==k {print substr($0, index($0, "=") + 1); exit}' "$config_path")
  if [[ -n "$line" ]]; then
    trim_spaces "$line"
    return 0
  fi
  return 1
}

extract_path_from_config() {
  local config_path="$1"
  local key
  for key in IndexDirectory VectorPath QueryPath TruthPath; do
    if extract_config_value "$config_path" "$key"; then
      return 0
    fi
  done
  return 1
}

existing_probe_path() {
  local p="$1"
  if [[ -z "$p" ]]; then
    return 1
  fi
  if [[ -e "$p" ]]; then
    printf '%s' "$p"
    return 0
  fi
  local parent="$p"
  while [[ "$parent" != "/" ]]; do
    parent=$(dirname "$parent")
    if [[ -e "$parent" ]]; then
      printf '%s' "$parent"
      return 0
    fi
  done
  printf '/'
}

detect_device_from_path() {
  local target_path="$1"
  local probe_path
  probe_path=$(existing_probe_path "$target_path")
  local source_device
  source_device=$(df -P "$probe_path" 2>/dev/null | awk 'NR==2 {print $1}')
  if [[ -z "$source_device" ]]; then
    return 1
  fi
  if [[ "$source_device" == /dev/* ]]; then
    source_device=$(readlink -f "$source_device")
  fi
  basename "$source_device"
}

device_exists_in_diskstats() {
  local device_name="$1"
  awk -v d="$device_name" '$3==d {found=1; exit} END {exit (found ? 0 : 1)}' /proc/diskstats
}

path_mountpoint() {
  local p="$1"
  findmnt -T "$p" -o TARGET -n 2>/dev/null || true
}

path_mount_options() {
  local p="$1"
  findmnt -T "$p" -o OPTIONS -n 2>/dev/null || true
}

path_is_read_only() {
  local p="$1"
  local opts
  opts=$(path_mount_options "$p")
  if [[ -z "$opts" ]]; then
    return 1
  fi
  awk -F, '{for (i=1; i<=NF; i++) if ($i=="ro") {found=1}} END {exit(found?0:1)}' <<<"$opts"
}

clear_system_cache() {
  if ! command -v sudo >/dev/null 2>&1; then
    echo "sudo is required for --clear-cache" >&2
    exit 1
  fi
  if ! sudo -n true 2>/dev/null; then
    echo "Passwordless sudo is required for --clear-cache." >&2
    echo "Hint: configure sudoers for drop_caches, or run without -C." >&2
    exit 1
  fi
  sync
  echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
  echo "System page cache cleared."
}

resolve_query_csv_path() {
  if [[ -n "$QUERY_CSV" ]]; then
    printf '%s' "$QUERY_CSV"
    return 0
  fi

  extract_config_value "$CONFIG" "DetailedIOStatsOutput" || true
}

ensure_output_targets_safe() {
  local query_csv_path="$1"
  local -a targets=(
    "$OUTPUT_DIR/sptag.log"
    "$OUTPUT_DIR/report.md"
    "$OUTPUT_DIR/disk_stats.csv"
    "$OUTPUT_DIR/process_io_stats.csv"
    "$OUTPUT_DIR/cpu_stats.csv"
    "$OUTPUT_DIR/psi_io_stats.csv"
  )

  if [[ -n "$query_csv_path" ]]; then
    targets+=("$query_csv_path")
  fi

  local target
  for target in "${targets[@]}"; do
    if [[ -e "$target" ]]; then
      echo "Refusing to overwrite existing output: $target" >&2
      echo "Please choose a fresh output namespace or remove the old files first." >&2
      exit 1
    fi
  done
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -b|--binary) BINARY="$2"; shift 2 ;;
    -c|--config) CONFIG="$2"; shift 2 ;;
    -d|--device) DEVICE="$2"; shift 2 ;;
    -p|--data-path) DATA_PATH="$2"; shift 2 ;;
    -o|--output-dir) OUTPUT_DIR="$2"; shift 2 ;;
    -m|--max-read-mbps) MAX_READ_MBPS="$2"; shift 2 ;;
    -q|--query-csv) QUERY_CSV="$2"; shift 2 ;;
    -i|--interval-ms) INTERVAL_MS="$2"; shift 2 ;;
    -C|--clear-cache) CLEAR_CACHE="true"; shift ;;
    -h|--help) usage; exit 0 ;;
    *)
      echo "Unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ -z "$CONFIG" || -z "$OUTPUT_DIR" ]]; then
  usage
  exit 1
fi

REF_PATH="$DATA_PATH"
if [[ -z "$REF_PATH" ]]; then
  REF_PATH=$(extract_path_from_config "$CONFIG" || true)
fi

if [[ -z "$DEVICE" ]]; then
  if [[ -z "$REF_PATH" ]]; then
    echo "Failed to auto-detect disk device: please pass -d <device> or -p <data-path>." >&2
    exit 1
  fi
  DEVICE=$(detect_device_from_path "$REF_PATH" || true)
  if [[ -z "$DEVICE" ]]; then
    echo "Failed to auto-detect disk device from path: $REF_PATH" >&2
    exit 1
  fi
  echo "Auto-detected disk device: $DEVICE (from path: $REF_PATH)"
elif [[ -n "$REF_PATH" ]]; then
  EXPECTED_DEVICE=$(detect_device_from_path "$REF_PATH" || true)
  if [[ -n "$EXPECTED_DEVICE" && "$EXPECTED_DEVICE" != "$DEVICE" ]]; then
    echo "Warning: explicit device '$DEVICE' differs from auto-detected '$EXPECTED_DEVICE' (path: $REF_PATH)." >&2
  fi
fi

if ! device_exists_in_diskstats "$DEVICE"; then
  echo "Device '$DEVICE' not found in /proc/diskstats. Please pass a valid -d <device>." >&2
  exit 1
fi

if [[ -n "$REF_PATH" ]] && path_is_read_only "$REF_PATH"; then
  MP=$(path_mountpoint "$REF_PATH")
  OPTS=$(path_mount_options "$REF_PATH")
  echo "Data path is on read-only mount: $MP (options: $OPTS)" >&2
  echo "Please remount it as read-write before benchmark, e.g.:" >&2
  echo "  sudo mount -o remount,rw $MP" >&2
  exit 1
fi

if [[ "$CLEAR_CACHE" == "true" ]]; then
  clear_system_cache
fi

QUERY_CSV=$(resolve_query_csv_path)
ensure_output_targets_safe "$QUERY_CSV"
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
