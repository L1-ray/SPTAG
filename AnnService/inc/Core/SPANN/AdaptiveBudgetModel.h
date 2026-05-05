// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <cmath>
#include <algorithm>
#include "json.hpp"  // nlohmann/json (local copy)

namespace SPTAG {
namespace SPANN {

/**
 * Simple GBDT model for adaptive budget prediction.
 * Loads models exported from LightGBM in JSON format.
 */
class AdaptiveBudgetModel {
public:
    struct TreeNode {
        int split_feature;      // Feature index to split on
        double threshold;       // Split threshold
        int left_child;         // Left child index (negative = leaf)
        int right_child;        // Right child index (negative = leaf)
        double leaf_value;      // Leaf value (only valid if this is a leaf)
    };

    struct Tree {
        std::vector<TreeNode> nodes;
        int num_leaves;
    };

    struct Model {
        std::vector<std::string> feature_names;
        int num_features;
        std::vector<Tree> trees;
        std::unordered_map<std::string, int> feature_index;  // feature_name -> index
    };

    AdaptiveBudgetModel() : m_threshold(0.97) {}

    /**
     * Load a risk model from JSON file.
     * @param modelPath Path to the JSON model file
     * @return true if loaded successfully
     */
    bool LoadModel(const std::string& modelPath) {
        std::ifstream file(modelPath);
        if (!file.is_open()) {
            return false;
        }

        try {
            nlohmann::json j;
            file >> j;

            m_model.feature_names = j["feature_names"].get<std::vector<std::string>>();
            m_model.num_features = j["num_features"].get<int>();
            m_model.trees.clear();

            // Build feature index map
            for (size_t i = 0; i < m_model.feature_names.size(); i++) {
                m_model.feature_index[m_model.feature_names[i]] = static_cast<int>(i);
            }

            // Parse trees
            for (const auto& treeJson : j["trees"]) {
                Tree tree;
                tree.num_leaves = treeJson["num_leaves"].get<int>();
                int num_nodes = treeJson["split_feature"].size();

                const auto& splitFeatures = treeJson["split_feature"];
                const auto& thresholds = treeJson["threshold"];
                const auto& leftChildren = treeJson["left_child"];
                const auto& rightChildren = treeJson["right_child"];
                const auto& leafValues = treeJson["leaf_value"];

                tree.nodes.resize(num_nodes);
                for (int i = 0; i < num_nodes; i++) {
                    tree.nodes[i].split_feature = splitFeatures[i].get<int>();
                    tree.nodes[i].threshold = thresholds[i].get<double>();
                    tree.nodes[i].left_child = leftChildren[i].get<int>();
                    tree.nodes[i].right_child = rightChildren[i].get<int>();
                }

                // Store leaf values separately for leaf nodes
                // In LightGBM format, leaf indices are negative: -(index + 1)
                // We'll handle this during prediction

                m_model.trees.push_back(std::move(tree));
            }

            // Store leaf values per tree
            for (size_t t = 0; t < j["trees"].size(); t++) {
                const auto& leafValues = j["trees"][t]["leaf_value"];
                m_leafValues.emplace_back(leafValues.get<std::vector<double>>());
            }

            return true;
        }
        catch (const std::exception& e) {
            return false;
        }
    }

    /**
     * Predict probability using the loaded model.
     * @param features Feature vector (must have num_features elements)
     * @return Predicted probability (after sigmoid)
     */
    double Predict(const std::vector<double>& features) const {
        if (features.size() != static_cast<size_t>(m_model.num_features)) {
            return 0.5;  // Invalid input, return default
        }

        double sum = 0.0;
        for (size_t t = 0; t < m_model.trees.size(); t++) {
            sum += PredictTree(t, features);
        }

        // Sigmoid for binary classification
        return 1.0 / (1.0 + std::exp(-sum));
    }

    /**
     * Predict for a single tree.
     */
    double PredictTree(size_t treeIdx, const std::vector<double>& features) const {
        const Tree& tree = m_model.trees[treeIdx];
        int node = 0;  // Start at root

        while (node >= 0 && node < static_cast<int>(tree.nodes.size())) {
            const TreeNode& n = tree.nodes[node];
            int featIdx = n.split_feature;

            if (featIdx < 0 || featIdx >= static_cast<int>(features.size())) {
                break;  // Invalid feature index
            }

            if (features[featIdx] <= n.threshold) {
                node = n.left_child;
            } else {
                node = n.right_child;
            }
        }

        // node is negative for leaf nodes: leaf_idx = -(node) - 1
        if (node < 0) {
            int leafIdx = -(node) - 1;
            if (leafIdx >= 0 && leafIdx < static_cast<int>(m_leafValues[treeIdx].size())) {
                return m_leafValues[treeIdx][leafIdx];
            }
        }

        return 0.0;  // Fallback
    }

    /**
     * Get feature index by name.
     */
    int GetFeatureIndex(const std::string& name) const {
        auto it = m_model.feature_index.find(name);
        if (it != m_model.feature_index.end()) {
            return it->second;
        }
        return -1;
    }

    /**
     * Get number of features.
     */
    int GetNumFeatures() const { return m_model.num_features; }

    /**
     * Get threshold for safe budget prediction.
     */
    double GetThreshold() const { return m_threshold; }
    void SetThreshold(double threshold) { m_threshold = threshold; }

    /**
     * Check if model is loaded.
     */
    bool IsLoaded() const { return !m_model.trees.empty(); }

private:
    Model m_model;
    std::vector<std::vector<double>> m_leafValues;  // Leaf values per tree
    double m_threshold;  // Probability threshold for "safe" prediction
};

/**
 * Adaptive budget predictor using risk-control models.
 * Uses separate models for candidate budgets to predict safety probability.
 */
class AdaptiveBudgetPredictor {
public:
    AdaptiveBudgetPredictor() : m_defaultBudget(64), m_threshold(0.97) {}

    /**
     * Load all risk models from directory.
     * @param modelDir Directory containing risk_model_b32.json, etc.
     * @param budgets List of budgets to load (e.g., {32, 40, 48, 56})
     * @return Number of models loaded successfully
     */
    int LoadModels(const std::string& modelDir, const std::vector<int>& budgets) {
        int loaded = 0;
        m_models.clear();
        m_budgets = budgets;

        for (int b : budgets) {
            std::string path = modelDir + "/risk_model_b" + std::to_string(b) + ".json";
            AdaptiveBudgetModel model;
            if (model.LoadModel(path)) {
                model.SetThreshold(m_threshold);
                m_models[b] = std::move(model);
                loaded++;
            }
        }

        // If no models loaded, try loading from current directory
        if (loaded == 0) {
            // Log this for debugging
            // SPTAGLIB_LOG would require including additional headers
        }

        return loaded;
    }

    /**
     * Predict the minimum safe budget for given features.
     * @param features Feature vector extracted from head distances
     * @return Minimum budget where P(safe) >= threshold, or default if none
     */
    int PredictBudget(const std::vector<double>& features) const {
        // Check budgets from smallest to largest
        for (int b : m_budgets) {
            auto it = m_models.find(b);
            if (it != m_models.end() && it->second.IsLoaded()) {
                double prob = it->second.Predict(features);
                if (prob >= GetThresholdForBudget(b)) {
                    return b;  // Safe to use this budget
                }
            }
        }
        return m_defaultBudget;  // Fallback to default
    }

    /**
     * Get probability for specific budget.
     */
    double GetSafetyProbability(int budget, const std::vector<double>& features) const {
        auto it = m_models.find(budget);
        if (it != m_models.end() && it->second.IsLoaded()) {
            return it->second.Predict(features);
        }
        return 0.0;
    }

    /**
     * Set threshold for safety prediction.
     */
    void SetThreshold(double threshold) {
        m_threshold = threshold;
        for (auto& pair : m_models) {
            pair.second.SetThreshold(threshold);
        }
    }

    double GetThreshold() const { return m_threshold; }

    /**
     * Set per-budget thresholds. Budgets not present here use global threshold.
     */
    void SetBudgetThresholds(const std::unordered_map<int, double>& thresholds) {
        m_budgetThresholds = thresholds;
    }

    /**
     * Set threshold for one budget.
     */
    void SetBudgetThreshold(int budget, double threshold) {
        m_budgetThresholds[budget] = threshold;
    }

    /**
     * Get threshold for one budget.
     */
    double GetThresholdForBudget(int budget) const {
        auto it = m_budgetThresholds.find(budget);
        if (it != m_budgetThresholds.end()) {
            return it->second;
        }
        return m_threshold;
    }

    /**
     * Set default budget (fallback).
     */
    void SetDefaultBudget(int budget) { m_defaultBudget = budget; }
    int GetDefaultBudget() const { return m_defaultBudget; }

    /**
     * Check if any models are loaded.
     */
    bool IsLoaded() const { return !m_models.empty(); }

private:
    std::unordered_map<int, AdaptiveBudgetModel> m_models;
    std::unordered_map<int, double> m_budgetThresholds;
    std::vector<int> m_budgets;
    int m_defaultBudget;
    double m_threshold;
};

/**
 * Feature extractor for adaptive budget.
 * Extracts features from head distances for model prediction.
 */
class AdaptiveBudgetFeatureExtractor {
public:
    /**
     * Extract features from head distances.
     * @param headDistances Distances from head search (sorted by distance)
     * @param features Output feature vector
     * @param internalResultNum InternalResultNum config (determines feature count)
     *
     * Feature order:
     * - ir=64 (24 features): d1, d2, d4, d8, d16, d32, d64, margin_2/4/8/16/32/64,
     *   ratio_8/16/64, slope_1_8/8_16/16_64, var_16/64, entropy_16/64, margin_16_32_ratio
     * - ir=128 (27 features): above + d96, d128, slope_64_96
     */
    static void Extract(const std::vector<float>& headDistances,
                       std::vector<double>& features,
                       int internalResultNum = 64) {
        bool isIR128 = (internalResultNum >= 128);
        int numFeatures = isIR128 ? 27 : 24;
        features.resize(numFeatures, 0.0);

        if (headDistances.empty()) {
            return;
        }

        int n = static_cast<int>(headDistances.size());

        // Raw distances: d1, d2, d4, d8, d16, d32, d64 (7 features, indices 0-6)
        // For ir=128: also d96, d128 (2 more features, indices 7-8)
        int distIndices64[] = {0, 1, 3, 7, 15, 31, 63};
        int distIndices128[] = {0, 1, 3, 7, 15, 31, 63, 95, 127};

        if (isIR128) {
            for (int i = 0; i < 9; i++) {
                if (distIndices128[i] < n) {
                    features[i] = static_cast<double>(headDistances[distIndices128[i]]);
                }
            }
        } else {
            for (int i = 0; i < 7; i++) {
                if (distIndices64[i] < n) {
                    features[i] = static_cast<double>(headDistances[distIndices64[i]]);
                }
            }
        }

        float d1 = headDistances[0];

        // Margins: margin_2, margin_4, margin_8, margin_16, margin_32, margin_64
        // ir=64: indices 7-12, ir=128: indices 9-14
        int marginIndices[] = {1, 3, 7, 15, 31, 63};
        int marginOffset = isIR128 ? 9 : 7;
        for (int i = 0; i < 6; i++) {
            int idx = marginIndices[i];
            if (idx < n && d1 > 0) {
                features[marginOffset + i] = (headDistances[idx] - d1) / d1;
            }
        }

        // Ratios: ratio_8, ratio_16, ratio_64
        // ir=64: indices 13-15, ir=128: indices 15-17
        int ratioIndices[] = {7, 15, 63};
        int ratioOffset = isIR128 ? 15 : 13;
        for (int i = 0; i < 3; i++) {
            int idx = ratioIndices[i];
            if (idx < n && d1 > 0) {
                features[ratioOffset + i] = headDistances[idx] / d1;
            }
        }

        // Slopes: slope_1_8, slope_8_16, slope_16_64 (+ slope_64_96 for ir=128)
        // ir=64: indices 16-18, ir=128: indices 18-21
        int slopeOffset = isIR128 ? 18 : 16;
        auto safeSlope = [&](int start, int end) -> float {
            if (end < n) {
                return (headDistances[end] - headDistances[start]) / (end - start);
            }
            return 0.0f;
        };
        features[slopeOffset] = safeSlope(0, 7);     // slope_1_8
        features[slopeOffset + 1] = safeSlope(7, 15); // slope_8_16
        features[slopeOffset + 2] = safeSlope(15, 63); // slope_16_64

        if (isIR128) {
            features[slopeOffset + 3] = safeSlope(63, 95); // slope_64_96
        }

        // Variance: var_16, var_64
        // ir=64: indices 19-20, ir=128: indices 22-23
        int varOffset = isIR128 ? 22 : 19;
        auto variance = [&](int count) -> float {
            int cnt = std::min(count, n);
            if (cnt < 2) return 0.0f;
            float mean = 0.0f;
            for (int i = 0; i < cnt; i++) mean += headDistances[i];
            mean /= cnt;
            float var = 0.0f;
            for (int i = 0; i < cnt; i++) {
                float diff = headDistances[i] - mean;
                var += diff * diff;
            }
            return var / cnt;
        };
        features[varOffset] = variance(16);     // var_16
        features[varOffset + 1] = variance(64); // var_64

        // Entropy: entropy_16, entropy_64
        // ir=64: indices 21-22, ir=128: indices 24-25
        int entropyOffset = isIR128 ? 24 : 21;
        auto entropy = [&](int count) -> float {
            int cnt = std::min(count, n);
            if (cnt < 2) return 0.0f;

            float maxVal = headDistances[0];
            for (int i = 1; i < cnt; i++) {
                if (headDistances[i] > maxVal) maxVal = headDistances[i];
            }

            float sum = 0.0f;
            std::vector<float> probs(cnt);
            for (int i = 0; i < cnt; i++) {
                probs[i] = std::exp(-(headDistances[i] - maxVal));
                sum += probs[i];
            }

            float ent = 0.0f;
            for (int i = 0; i < cnt; i++) {
                float p = probs[i] / sum;
                if (p > 1e-10f) {
                    ent -= p * std::log(p);
                }
            }
            return ent;
        };
        features[entropyOffset] = entropy(16);     // entropy_16
        features[entropyOffset + 1] = entropy(64); // entropy_64

        // margin_16_32_ratio
        // ir=64: index 23, ir=128: index 26
        int lastIdx = isIR128 ? 26 : 23;
        float margin16 = features[marginOffset + 3];  // margin_16
        float margin32 = features[marginOffset + 4];  // margin_32
        if (margin32 > 0) {
            features[lastIdx] = margin16 / margin32;
        }
    }

    /**
     * Get feature names for debugging/logging.
     */
    static std::vector<std::string> GetFeatureNames(int internalResultNum = 64) {
        bool isIR128 = (internalResultNum >= 128);
        if (isIR128) {
            return {
                "d1", "d2", "d4", "d8", "d16", "d32", "d64", "d96", "d128",
                "margin_2", "margin_4", "margin_8", "margin_16", "margin_32", "margin_64",
                "ratio_8", "ratio_16", "ratio_64",
                "slope_1_8", "slope_8_16", "slope_16_64", "slope_64_96",
                "var_16", "var_64",
                "entropy_16", "entropy_64",
                "margin_16_32_ratio"
            };
        } else {
            return {
                "d1", "d2", "d4", "d8", "d16", "d32", "d64",
                "margin_2", "margin_4", "margin_8", "margin_16", "margin_32", "margin_64",
                "ratio_8", "ratio_16", "ratio_64",
                "slope_1_8", "slope_8_16", "slope_16_64",
                "var_16", "var_64",
                "entropy_16", "entropy_64",
                "margin_16_32_ratio"
            };
        }
    }
};

} // namespace SPANN
} // namespace SPTAG
