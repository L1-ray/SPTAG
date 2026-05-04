#!/usr/bin/env Python3
"""
Export LightGBM risk models to JSON for C++ inference.
"""

import json
import numpy as np


def parse_tree(tree_text):
    """Parse a single tree from LightGBM text format."""
    lines = tree_text.strip().split('\n')

    tree = {
        'num_leaves': 0,
        'split_feature': [],
        'threshold': [],
        'left_child': [],
        'right_child': [],
        'leaf_value': [],
    }

    for line in lines:
        if line.startswith('num_leaves='):
            tree['num_leaves'] = int(line.split('=')[1])
        elif line.startswith('split_feature='):
            tree['split_feature'] = [int(x) for x in line.split('=')[1].split()]
        elif line.startswith('threshold='):
            tree['threshold'] = [float(x) for x in line.split('=')[1].split()]
        elif line.startswith('left_child='):
            tree['left_child'] = [int(x) for x in line.split('=')[1].split()]
        elif line.startswith('right_child='):
            tree['right_child'] = [int(x) for x in line.split('=')[1].split()]
        elif line.startswith('leaf_value='):
            tree['leaf_value'] = [float(x) for x in line.split('=')[1].split()]

    return tree


def export_model_to_json(model_path, output_path):
    """Export LightGBM model to JSON format for C++ inference."""

    with open(model_path, 'r') as f:
        model_text = f.read()

    # Parse header
    lines = model_text.split('\n')
    feature_names = []

    for line in lines:
        if line.startswith('feature_names='):
            feature_names = line.split('=')[1].split()
            break

    # Parse trees
    trees = []
    tree_texts = model_text.split('Tree=')[1:]  # Skip header

    for tree_text in tree_texts:
        tree = parse_tree(tree_text)
        trees.append(tree)

    # Create output
    output = {
        'feature_names': feature_names,
        'num_features': len(feature_names),
        'num_trees': len(trees),
        'trees': trees,
    }

    with open(output_path, 'w') as f:
        json.dump(output, f, indent=2)

    print(f"Exported {model_path} -> {output_path}")
    print(f"  Features: {len(feature_names)}")
    print(f"  Trees: {len(trees)}")

    return output


def verify_export(json_path):
    """Verify the exported model produces valid predictions."""

    # Load exported model
    with open(json_path, 'r') as f:
        exported = json.load(f)

    # Create test data
    np.random.seed(42)
    n_samples = 10
    n_features = exported['num_features']
    X = np.random.randn(n_samples, n_features)

    # Exported model predictions (simple tree ensemble)
    exported_pred = predict_with_exported(exported, X)

    print(f"\nVerification:")
    print(f"  Sample predictions: {exported_pred[:5]}")
    print(f"  Range: [{exported_pred.min():.4f}, {exported_pred.max():.4f}]")


def predict_with_exported(model, X):
    """Predict using exported model structure."""
    n_samples = X.shape[0]
    predictions = np.zeros(n_samples)

    for tree in model['trees']:
        leaf_values = np.zeros(n_samples)
        for i in range(n_samples):
            leaf_values[i] = predict_single_tree(tree, X[i])
        predictions += leaf_values

    # Sigmoid for binary classification
    return 1.0 / (1.0 + np.exp(-predictions))


def predict_single_tree(tree, x):
    """Traverse a single tree to get leaf value."""
    node = 0  # Start at root

    # Negative values in left_child/right_child indicate leaf nodes
    # Leaf index = -(child) - 1
    while node >= 0:
        feature_idx = tree['split_feature'][node]
        threshold = tree['threshold'][node]

        if x[feature_idx] <= threshold:
            node = tree['left_child'][node]
        else:
            node = tree['right_child'][node]

    # node is negative, convert to leaf index
    leaf_idx = -node - 1
    return tree['leaf_value'][leaf_idx]


def main():
    import sys

    if len(sys.argv) > 1:
        # Accept directory path as argument
        model_dir = sys.argv[1]
    else:
        model_dir = 'results/adaptive_budget/phase4_learned'

    budgets = [32, 40, 48]

    for b in budgets:
        model_path = f'{model_dir}/risk_model_b{b}.txt'
        json_path = f'{model_dir}/risk_model_b{b}.json'

        try:
            export_model_to_json(model_path, json_path)
            verify_export(json_path)
        except Exception as e:
            print(f"Error processing B={b}: {e}")


if __name__ == '__main__':
    main()
