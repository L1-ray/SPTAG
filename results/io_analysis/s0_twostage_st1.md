# SPANN S0 Diagnosis

## Snapshot

- Query count: `10000`
- Avg recall: `0.976780`
- Avg total latency: `3.970 ms`
- Approx single-thread QPS: `251.90`
- Payload wait / ex latency: `0.561`
- Avg candidates per unique payload page: `4.197`

## Concentration

- Top10 query share of payload wait: `0.123`
- Gini(payload wait per query): `0.082`

## Direction Ranking

- `M1` score `2`
- `M2-H` score `2`
- `M4` score `1`

## Why

- payload_read_wait_over_ex is high; read wait is likely dominant
- candidates per unique payload page is low; page fanout/locality is weak
