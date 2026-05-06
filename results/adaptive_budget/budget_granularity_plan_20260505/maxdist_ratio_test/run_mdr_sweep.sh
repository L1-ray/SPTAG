#!/bin/bash

# Test MaxDistRatio parameter sweep for baseline configuration
# st=8, nt=16, ir=64, pl=4

SCRIPT_DIR="/home/ray/code/SPTAG/results/adaptive_budget/budget_granularity_plan_20260505/maxdist_ratio_test"
TEMPLATE="$SCRIPT_DIR/baseline_template.ini"
SPTAG_BIN="/home/ray/code/SPTAG/Release/ssdserving"

# MaxDistRatio values to test
MDR_VALUES=(1000000 7 6 5 4 3 2 1)

cd /home/ray/code/SPTAG

# Clear disk cache before each test
clear_cache() {
    echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
    sync
}

for mdr in "${MDR_VALUES[@]}"; do
    echo "=========================================="
    echo "Testing MaxDistRatio=$mdr"
    echo "=========================================="

    # Create config file
    config_file="$SCRIPT_DIR/baseline_mdr${mdr}.ini"
    sed "s/MAXDIST_RATIO_PLACEHOLDER/$mdr/g" "$TEMPLATE" > "$config_file"

    # Update output paths
    sed -i "s/mdr_RESULT/mdr${mdr}_result/g" "$config_file"
    sed -i "s/mdr\.csv/mdr${mdr}.csv/g" "$config_file"

    # Clear cache
    echo "Clearing disk cache..."
    clear_cache

    # Run test
    echo "Running test..."
    $SPTAG_BIN "$config_file" 2>&1 | tee "$SCRIPT_DIR/baseline_mdr${mdr}.log"

    echo ""
done

echo "=========================================="
echo "All tests completed!"
echo "=========================================="
