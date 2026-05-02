# SPANN S0 Diagnosis

## Snapshot

- Query count: `10000`
- Avg recall: `0.976780`
- Avg total latency: `4.426 ms`
- Approx single-thread QPS: `225.92`
- Payload wait / ex latency: `0.598`
- Avg candidates per unique payload page: `4.197`

## Concentration

- Top10 query share of payload wait: `0.137`
- Gini(payload wait per query): `0.099`
- Trace rows: `5119900`
- Top10 page-hit share: `0.474`
- Top10 posting-hit share: `0.413`
- Cross-query reuse page ratio: `1.000`

## Direction Ranking

- `M2-H` score `5`
- `M1` score `4`
- `M4` score `2`

## Why

- payload_read_wait_over_ex is high; read wait is likely dominant
- candidates per unique payload page is low; page fanout/locality is weak
- page hotness concentration is high; cache/coalescing should have room
- cross-query page reuse is visible; global page broker can exploit sharing
