# SIFT1M A3 vs A4_cons_v3 Repeat Report

## Per-run QPS/Recall

```csv
mode,rep,qps,recall10
A3_ref,1,7434.94,0.97632
A4_cons_v3,1,7490.64,0.97638
A3_ref,2,7429.42,0.97632
A4_cons_v3,2,7412.9,0.97642
A3_ref,3,7407.41,0.97632
A4_cons_v3,3,7451.56,0.9764
A3_ref,4,7451.56,0.9763
A4_cons_v3,4,7434.94,0.9764
A3_ref,5,7479.43,0.9763
A4_cons_v3,5,7457.12,0.9764
```

## Mean/Std + IO Metrics

```csv
mode,qps_mean,qps_std,recall_mean,recall_std,avg_pages_mean,avg_postings_mean,low07_mean,low05_mean
A3_ref,7440.552000000001,26.86300001861295,0.9763119999999998,1.0954451150104143e-05,92.85376,48.91678,24.0,2.0
A4_cons_v3,7449.432000000001,28.750382953971727,0.9763999999999999,1.4142135623725467e-05,92.39688,48.5362,24.0,2.0
```

## Delta (A4_cons_v3 - A3_ref)

```csv
delta_qps_pct,delta_recall,pages_reduction_pct,postings_reduction_pct,delta_low07,delta_low05
0.1193459840076396,8.800000000008801e-05,0.4920425408728717,0.7780152332185436,0.0,0.0
```
