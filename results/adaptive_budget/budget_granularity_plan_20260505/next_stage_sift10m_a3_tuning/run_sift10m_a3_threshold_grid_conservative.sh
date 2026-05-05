#!/usr/bin/env bash
set -euo pipefail
BASE=/home/ray/code/SPTAG
BIN=$BASE/Release/ssdserving
OUT=$BASE/results/adaptive_budget/budget_granularity_plan_20260505/next_stage_sift10m_a3_tuning
mkdir -p "$OUT"

# More conservative than A3_ref (0.95,0.92,0.88,0.82)
GRID=(
  "0.95 0.93 0.89 0.83"
  "0.95 0.94 0.90 0.84"
  "0.95 0.95 0.91 0.85"
  "0.96 0.94 0.90 0.84"
  "0.96 0.95 0.91 0.85"
  "0.97 0.95 0.92 0.86"
  "0.98 0.96 0.93 0.87"
)

echo "tag,t32,t40,t48,t56,qps,recall10" > "$OUT/grid_raw_conservative.csv"

run_one() {
  local tag="$1"; local t32="$2"; local t40="$3"; local t48="$4"; local t56="$5"
  local ini="$OUT/${tag}.ini"
  local csv="$OUT/${tag}.csv"
  local log="$OUT/${tag}.log"

  cat > "$ini" << EOINI
[Base]
ValueType=UInt8
DistCalcMethod=L2
IndexAlgoType=BKT
Dim=128
VectorPath=/home/ray/data/sift10m/bigann10m_base.u8bin
VectorType=DEFAULT
VectorSize=10000000
QueryPath=/home/ray/data/sift10m/query.public.10K.u8bin
QueryType=DEFAULT
QuerySize=10000
TruthPath=/home/ray/data/sift10m/bigann-10M.bin
TruthType=DEFAULT
GenerateTruth=false
IndexDirectory=/home/ray/data/sift10m/spann_index_u8default_20260430
SearchResult=${OUT}/${tag}.bin
HeadIndexFolder=head_index

[SelectHead]
isExecute=false

[BuildHead]
isExecute=false

[SearchSSDIndex]
isExecute=true
BuildSsdIndex=false
InternalResultNum=64
NumberOfThreads=16
SearchThreadNum=8
ResultNum=10
MaxDistRatio=1000000
SearchPostingPageLimit=4
EnableDetailedIOStats=true
DetailedIOStatsOutput=${csv}
DetailedIOStatsSampleRate=1.0
EnablePageCache=false
EnableInFlightCoalescing=true
EnableLearnedBudget=true
LearnedBudgetModelPath=/home/ray/code/SPTAG/results/adaptive_budget/sift10m_ir64_retrain
LearnedBudgetThreshold=0.85
LearnedBudgetCandidates=32,40,48,56
LearnedBudgetThresholds=32:${t32},40:${t40},48:${t48},56:${t56}
LearnedBudgetDefault=64
LearnedBudgetMin=32
EOINI

  sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
  "$BIN" "$ini" > "$log" 2>&1
  local qps recall
  qps=$(rg -o "actuallQPS is [0-9.]+" "$log" | awk '{print $3}' | tail -n1)
  recall=$(rg -o "Recall@10: [0-9.]+" "$log" | awk '{print $2}' | tail -n1)
  echo "$tag,$t32,$t40,$t48,$t56,$qps,$recall" >> "$OUT/grid_raw_conservative.csv"
}

idx=0
for g in "${GRID[@]}"; do
  idx=$((idx+1))
  read -r t32 t40 t48 t56 <<< "$g"
  tag=$(printf "c%02d_t32_%s_t40_%s_t48_%s_t56_%s" "$idx" "$t32" "$t40" "$t48" "$t56" | tr '.' 'p')
  run_one "$tag" "$t32" "$t40" "$t48" "$t56"
done

python3 - << 'PY'
import pandas as pd
from pathlib import Path
import re
out=Path('/home/ray/code/SPTAG/results/adaptive_budget/budget_granularity_plan_20260505/next_stage_sift10m_a3_tuning')
raw=pd.read_csv(out/'grid_raw_conservative.csv')

def parse_log(p):
    txt=Path(p).read_text()
    qps=float(re.findall(r'actuallQPS is ([0-9.]+)',txt)[-1])
    recall=float(re.findall(r'Recall@10: ([0-9.]+)',txt)[-1])
    return qps,recall

b_qps,b_rec=parse_log(out/'baseline_b64.log')
a_qps,a_rec=parse_log(out/'a3_ref.log')

rows=[]
for _,r in raw.iterrows():
    tag=r['tag']
    df=pd.read_csv(out/f'{tag}.csv')
    rows.append({
        **r.to_dict(),
        'avg_pages':df['pages_read'].mean(),
        'avg_postings':df['postings_touched'].mean(),
        'low07':int((df['recall']<0.7).sum()),
        'low05':int((df['recall']<0.5).sum()),
        'delta_qps_vs_a3_pct':100*(r['qps']-a_qps)/a_qps,
        'delta_recall_vs_a3':r['recall10']-a_rec,
        'delta_qps_vs_b64_pct':100*(r['qps']-b_qps)/b_qps,
        'delta_recall_vs_b64':r['recall10']-b_rec,
    })
res=pd.DataFrame(rows)
res=res.sort_values('delta_qps_vs_b64_pct',ascending=False)
res.to_csv(out/'grid_enriched_conservative.csv',index=False)
cons=res[res['delta_recall_vs_b64']>=-0.0015].sort_values('delta_qps_vs_b64_pct',ascending=False)
cons.to_csv(out/'grid_constrained_conservative.csv',index=False)

print('Conservative top by qps vs b64:')
print(res[['tag','qps','recall10','delta_qps_vs_b64_pct','delta_recall_vs_b64']].head(7))
print('\nConstrained feasible:')
print(cons[['tag','qps','recall10','delta_qps_vs_b64_pct','delta_recall_vs_b64']].head(7))
PY
