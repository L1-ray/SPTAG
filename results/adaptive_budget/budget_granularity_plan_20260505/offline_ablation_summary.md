# Offline Ablation Summary (A0/A1/A2/A3)

Output time: 2026-05-05 11:17:08.819686

## Core Metrics

```csv
dataset,policy,pages_saving_pct,recall10_delta_vs_b64,miss_rate_pct,low_recall_lt_0_7,low_recall_lt_0_5,hard_oracle64_underbudget,oracle_util_pct,b32_pct,b40_pct,b48_pct,b56_pct,b64_pct
sift1m,A0_current,17.972797101079443,-0.0033999999999999586,3.0,2,0,14,34.67835249513775,22.3,12.8,15.65,0.0,49.25
sift1m,A1_add56_globalT,21.500752800673705,-0.005350000000000077,4.75,4,0,14,41.48551170634435,22.3,12.8,15.65,28.55,20.7
sift1m,A2_perT,18.49592977262869,-0.00385000000000002,3.3,2,0,21,35.68773234200746,15.05,16.05,27.0,0.0,41.9
sift1m,A3_add56_perT,22.954891503134544,-0.006700000000000039,5.9,5,0,21,44.29125942703333,15.05,16.05,27.0,35.6,6.3
sift10m,A0_current,18.060451600609756,-0.00759999999999994,6.75,38,1,38,41.614071675539215,21.75,11.65,19.9,0.0,46.7
sift10m,A1_add56_globalT,22.557005843495936,-0.01175000000000015,10.7,44,1,38,51.974827579898665,21.75,11.65,19.9,33.8,12.9
sift10m,A2_perT,13.817565421747963,-0.004650000000000154,4.3,38,1,27,31.837806194317903,10.6,11.35,24.8,0.0,53.25
sift10m,A3_add56_perT,19.62056974085365,-0.010250000000000092,9.6,45,2,27,45.20882498216343,10.6,11.35,24.8,43.65,9.6
```


## Plan Criteria Check

### sift1m

- A0 pages saving: 17.9728%
- A3 pages saving: 22.9549%
- Saving gain (A3-A0): 4.9821 pp
- A3 recall delta vs B64: -0.006700
- A3 miss rate: 5.9000%
- A3 oracle utilization: 44.29%
- Offline QPS proxy (save gain): 4.9821 pp

### sift10m

- A0 pages saving: 18.0605%
- A3 pages saving: 19.6206%
- Saving gain (A3-A0): 1.5601 pp
- A3 recall delta vs B64: -0.010250
- A3 miss rate: 9.6000%
- A3 oracle utilization: 45.21%
- Offline QPS proxy (save gain): 1.5601 pp

