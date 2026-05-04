#!/usr/bin/env python3
"""
Measure model inference overhead.
"""

import json
import time
import numpy as np

def load_model(path):
    with open(path, 'r') as f:
        return json.load(f)

def predict(model, features):
    """Simple GBDT prediction."""
    sum_val = 0.0
    for tree in model['trees']:
        node = 0
        while node >= 0:
            feat_idx = tree['split_feature'][node]
            threshold = tree['threshold'][node]
            if features[feat_idx] <= threshold:
                node = tree['left_child'][node]
            else:
                node = tree['right_child'][node]
        # Leaf node
        leaf_idx = -node - 1
        sum_val += tree['leaf_value'][leaf_idx]
    return 1.0 / (1.0 + np.exp(-sum_val))

def extract_features(head_distances):
    """Extract 27 features from head distances."""
    features = [0.0] * 27

    if len(head_distances) == 0:
        return features

    # d1, d2, d4, d8, d16, d32, d64, d96, d128
    indices = [0, 1, 3, 7, 15, 31, 63, 95, 127]
    for i, idx in enumerate(indices):
        if idx < len(head_distances):
            features[i] = head_distances[idx]

    d1 = head_distances[0] if head_distances[0] > 0.001 else 0.001

    # margins
    for i, idx in enumerate([1, 3, 7, 15, 31, 63]):
        if idx < len(head_distances) and d1 > 0:
            features[9 + i] = (head_distances[idx] - d1) / d1

    # ratios
    for i, idx in enumerate([7, 15, 63]):
        if idx < len(head_distances) and d1 > 0:
            features[15 + i] = head_distances[idx] / d1

    # slopes
    if len(head_distances) >= 8:
        features[18] = (head_distances[7] - head_distances[0]) / 7
    if len(head_distances) >= 16:
        features[19] = (head_distances[15] - head_distances[7]) / 8
    if len(head_distances) >= 64:
        features[20] = (head_distances[63] - head_distances[15]) / 48
    if len(head_distances) >= 96:
        features[21] = (head_distances[95] - head_distances[63]) / 32

    # variance
    if len(head_distances) >= 16:
        features[22] = np.var(head_distances[:16])
    if len(head_distances) >= 64:
        features[23] = np.var(head_distances[:64])

    # entropy
    for n in [16, 64]:
        if len(head_distances) >= n:
            neg_dists = -np.array(head_distances[:n])
            exp_dists = np.exp(neg_dists - np.max(neg_dists))
            softmax = exp_dists / exp_dists.sum()
            entropy = -np.sum(softmax * np.log(softmax + 1e-10))
            features[24 if n == 16 else 25] = entropy

    # margin_16_32_ratio
    if features[13] > 0:
        features[26] = features[12] / features[13]

    return features

def main():
    # Load models
    models = {}
    for b in [32, 40, 48]:
        with open(f'results/adaptive_budget/phase4_learned/risk_model_b{b}.json', 'r') as f:
            models[b] = json.load(f)

    print(f"Loaded {len(models)} models")
    print(f"Each model has {len(models[32]['trees'])} trees")

    # Generate random test data
    np.random.seed(42)
    n_queries = 10000
    test_queries = [np.sort(np.random.exponential(1000, 128)).tolist() for _ in range(n_queries)]

    # Warmup
    for _ in range(100):
        features = extract_features(test_queries[0])
        for b in [32, 40, 48]:
            predict(models[b], features)

    # Measure feature extraction
    start = time.perf_counter()
    for q in test_queries:
        extract_features(q)
    feature_time = (time.perf_counter() - start) / n_queries * 1000

    # Measure prediction (all 3 models)
    features_list = [extract_features(q) for q in test_queries[:1000]]
    start = time.perf_counter()
    for f in features_list:
        for b in [32, 40, 48]:
            predict(models[b], f)
    predict_time = (time.perf_counter() - start) / 1000 * 1000

    # Measure full pipeline
    start = time.perf_counter()
    for q in test_queries:
        features = extract_features(q)
        for b in [32, 40, 48]:
            predict(models[b], features)
    full_time = (time.perf_counter() - start) / n_queries * 1000

    print(f"\n=== Inference Overhead ===")
    print(f"Feature extraction: {feature_time:.4f} ms/query")
    print(f"Prediction (3 models): {predict_time:.4f} ms/query")
    print(f"Total overhead: {full_time:.4f} ms/query")
    print(f"\nAs % of query latency (0.34 ms): {100*full_time/0.34:.1f}%")

if __name__ == '__main__':
    main()
