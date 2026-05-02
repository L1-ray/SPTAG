#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ANALYZER="$ROOT_DIR/scripts/analyze_spann_s0_signals.py"

if [[ ! -f "$ANALYZER" ]]; then
  echo "Analyzer not found: $ANALYZER" >&2
  exit 1
fi

INPUT_ROOT="${1:-$ROOT_DIR/results/io_analysis/sift1m_official_u8default_20260430/sweeps_20260430_strict_v2}"
OUTPUT_ROOT="${2:-$ROOT_DIR/results/io_analysis/s0_baseline_20260502}"

mkdir -p "$OUTPUT_ROOT"

count=0
while IFS= read -r query_csv; do
  run_dir="$(dirname "$query_csv")"
  run_name="$(basename "$run_dir")"
  out_json="$OUTPUT_ROOT/${run_name}_s0_summary.json"
  out_md="$OUTPUT_ROOT/${run_name}_s0_report.md"
  title="S0 Diagnosis: ${run_name}"

  python3 "$ANALYZER" \
    --query-csv "$query_csv" \
    --output-json "$out_json" \
    --output-md "$out_md" \
    --title "$title"

  echo "generated: $out_json"
  echo "generated: $out_md"
  count=$((count + 1))
done < <(find "$INPUT_ROOT" -type f -name "query_io_stats.csv" | sort)

if [[ "$count" -eq 0 ]]; then
  echo "No query_io_stats.csv found under: $INPUT_ROOT" >&2
  exit 2
fi

echo "S0 diagnosis completed. runs=$count output=$OUTPUT_ROOT"
