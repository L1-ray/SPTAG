# S0 Diagnosis: st16_nt16_ir64_pl4

## Snapshot

- Query count: `10000`
- Avg recall: `0.977820`
- Avg total latency: `2.695 ms`
- Approx single-thread QPS: `371.08`
- Payload wait / ex latency: `N/A`
- Avg candidates per unique payload page: `N/A`

## Concentration

- Top10 query share of payload wait: `N/A`
- Gini(payload wait per query): `N/A`

## Direction Ranking

- `M4` score `1`
- `M1` score `0`
- `M2-H` score `0`

## Why

- payload-locality columns are unavailable/zero in this run; provide payload trace or richer instrumentation
- payload read wait columns are unavailable/zero; M1 scoring is conservative
