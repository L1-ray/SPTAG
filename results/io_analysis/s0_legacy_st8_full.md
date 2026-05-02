# SPANN S0 Diagnosis

## Snapshot

- Query count: `10000`
- Avg recall: `0.977820`
- Avg total latency: `1.434 ms`
- Approx single-thread QPS: `697.40`
- Payload wait / ex latency: `N/A`
- Avg candidates per unique payload page: `N/A`

## Concentration

- Top10 query share of payload wait: `N/A`
- Gini(payload wait per query): `N/A`
- Trace rows: `23142851`
- Top10 page-hit share: `0.286`
- Top10 posting-hit share: `0.335`
- Cross-query reuse page ratio: `0.924`

## Direction Ranking

- `M1` score `2`
- `M2-H` score `2`
- `M4` score `2`

## Why

- payload-locality columns are unavailable/zero in this run; provide payload trace or richer instrumentation
- payload read wait columns are unavailable/zero; M1 scoring is conservative
- cross-query page reuse is visible; global page broker can exploit sharing
