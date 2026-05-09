#!/usr/bin/env python3
"""
从 SIFT1M base 中随机采样 100K 向量作为训练集。
"""

import numpy as np
import struct
import os

# 配置
SEED = 42
BASE_PATH = '/home/ray/data/sift1m/bigann1m_base.u8bin'
OUTPUT_PATH = '/home/ray/data/sift1m/sift_base_sampled_100k.u8bin'
SAMPLE_SIZE = 100000

def read_u8bin(path):
    """读取 u8bin 格式文件"""
    with open(path, 'rb') as f:
        n = struct.unpack('i', f.read(4))[0]
        dim = struct.unpack('i', f.read(4))[0]
        data = np.frombuffer(f.read(n * dim), dtype=np.uint8)
        data = data.reshape(n, dim)
    print(f"Read {path}: n={n}, dim={dim}")
    return data, n, dim

def write_u8bin(path, data):
    """写入 u8bin 格式文件"""
    n, dim = data.shape
    with open(path, 'wb') as f:
        f.write(struct.pack('i', n))
        f.write(struct.pack('i', dim))
        f.write(data.tobytes())
    print(f"Wrote {path}: n={n}, dim={dim}")

def main():
    np.random.seed(SEED)

    # 1. 读取 base
    print("=== Step 1: Read base ===")
    base, n_base, dim = read_u8bin(BASE_PATH)

    # 2. 随机采样 100K
    print("\n=== Step 2: Sample from base ===")
    indices = np.random.choice(n_base, SAMPLE_SIZE, replace=False)
    sampled = base[indices]
    print(f"Sampled {SAMPLE_SIZE} vectors from base")
    print(f"Sample shape: {sampled.shape}, dtype: {sampled.dtype}")

    # 3. 保存
    print("\n=== Step 3: Save ===")
    write_u8bin(OUTPUT_PATH, sampled)

    print("\n=== Done ===")
    print(f"Created: {OUTPUT_PATH}")

if __name__ == '__main__':
    main()
