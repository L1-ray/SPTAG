# A4 Stage Gate (SIFT1M)

Date: 2026-05-05

## Decision

- Recommendation: **Keep A3_ref as default, use A4_cons_v3 as opt-in**
- Reason: A4 is safe on recall/tail, but QPS gain is small and condition-dependent.

## Aggregated Delta (A4_cons_v3 - A3_ref)

- Mean QPS delta: `0.224%`
- Mean Recall delta: `0.000086`
- Mean pages/query delta: `0.504%` (positive means fewer pages)
- Conditions with negative QPS delta: `2/4`

## Per-condition Delta

```csv
st,cache_mode,delta_qps_pct,delta_recall,pages_reduction_pct,postings_reduction_pct,delta_low07,delta_low05
8,cold,0.3432308238022963,7.800000000013352e-05,0.8229304227595574,1.1270755836831303,0.0,0.0
8,hot,-0.2778965427190185,8.000000000008001e-05,0.7071849496791927,1.010537350140098,0.0,0.0
16,cold,-0.0300999097804712,0.0001039999999999,-0.1380256275805442,0.1313456873052407,0.0,0.0
16,hot,0.8592399488472461,8.000000000008001e-05,0.6226716430714886,0.9282226319506172,0.0,0.0
```

## Ops Commands

- Run A3 (cold cache): `./scripts/run_sift1m_profile.sh a3 cold`
- Run A4 (cold cache): `./scripts/run_sift1m_profile.sh a4 cold`
- Run A3 (hot cache): `./scripts/run_sift1m_profile.sh a3 hot`
- Run A4 (hot cache): `./scripts/run_sift1m_profile.sh a4 hot`
