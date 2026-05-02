# SPANN S0 Diagnosis

## Snapshot

- Query count: `10000`
- Avg recall: `0.976780`
- Avg total latency: `4.412 ms`
- Approx single-thread QPS: `226.65`
- Payload wait / ex latency: `0.602`
- Avg candidates per unique payload page: `4.197`

## Concentration

- Top10 query share of payload wait: `0.137`
- Gini(payload wait per query): `0.100`

## Direction Ranking

- `M1` score `2`
- `M2-H` score `2`
- `M4` score `1`

## Why

- payload_read_wait_over_ex is high; read wait is likely dominant
- candidates per unique payload page is low; page fanout/locality is weak
