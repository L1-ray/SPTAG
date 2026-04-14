#!/usr/bin/env python3
"""
将 SIFT1M 的 ground truth 文件转换为 SPTAG 兼容格式

SIFT1M ground truth 格式 (ivecs):
- 每个向量: [K(int32)][id1(int32)][id2(int32)]...[idK(int32)]

SPTAG 支持的格式:
1. DEFAULT 格式 (文件名包含 "bin"):
   - 文件头: [query_count(int32)][K(int32)]
   - 数据: query_count * K 个 int32 (所有 truth 数据)
   - 距离: query_count * K 个 float32 (可选，这里填 0)

2. XVEC 格式:
   - 每个向量: [K(int32)][id1(int32)]...[idK(int32)]
   - 与 SIFT1M 格式相同！

3. TXT 格式:
   - 每行: id1 id2 ... idK
"""

import struct
import sys
import os

def read_ivecs(filepath):
    """读取 ivecs 格式文件"""
    vectors = []
    with open(filepath, 'rb') as f:
        while True:
            dim_bytes = f.read(4)
            if len(dim_bytes) < 4:
                break
            dim = struct.unpack('i', dim_bytes)[0]
            vec = struct.unpack(f'{dim}i', f.read(dim * 4))
            vectors.append(vec)
    return vectors

def write_sptag_default(filepath, vectors, k=None):
    """写入 SPTAG DEFAULT 格式"""
    query_count = len(vectors)
    if k is None:
        k = len(vectors[0])

    with open(filepath, 'wb') as f:
        # 写入文件头
        f.write(struct.pack('i', query_count))
        f.write(struct.pack('i', k))

        # 写入 truth 数据 (每个向量取前 k 个)
        for vec in vectors:
            for i in range(min(k, len(vec))):
                f.write(struct.pack('i', vec[i]))
            # 如果向量长度不足 k，补 0
            for i in range(len(vec), k):
                f.write(struct.pack('i', 0))

        # 写入距离数据 (全 0)
        for _ in range(query_count):
            for _ in range(k):
                f.write(struct.pack('f', 0.0))

def write_sptag_xvec(filepath, vectors, k=None):
    """写入 SPTAG XVEC 格式 (与原始 ivecs 相同)"""
    if k is None:
        k = len(vectors[0])

    with open(filepath, 'wb') as f:
        for vec in vectors:
            actual_k = min(k, len(vec))
            f.write(struct.pack('i', actual_k))
            for i in range(actual_k):
                f.write(struct.pack('i', vec[i]))

def write_sptag_txt(filepath, vectors, k=None):
    """写入 SPTAG TXT 格式"""
    if k is None:
        k = len(vectors[0])

    with open(filepath, 'w') as f:
        for vec in vectors:
            actual_k = min(k, len(vec))
            line = ' '.join(str(vec[i]) for i in range(actual_k))
            f.write(line + '\n')

def main():
    if len(sys.argv) < 3:
        print("用法: python convert_groundtruth.py <input.ivecs> <output_prefix> [--format FORMAT] [--k K]")
        print("格式选项: default, xvec, txt")
        print("示例: python convert_groundtruth.py sift_groundtruth.ivecs sift_groundtruth --format default --k 100")
        sys.exit(1)

    input_file = sys.argv[1]
    output_prefix = sys.argv[2]
    format_type = 'default'
    k = None

    i = 3
    while i < len(sys.argv):
        if sys.argv[i] == '--format':
            format_type = sys.argv[i + 1]
            i += 2
        elif sys.argv[i] == '--k':
            k = int(sys.argv[i + 1])
            i += 2
        else:
            i += 1

    print(f"读取文件: {input_file}")
    vectors = read_ivecs(input_file)
    print(f"读取到 {len(vectors)} 个向量，每个向量 {len(vectors[0])} 个结果")

    if format_type == 'default':
        output_file = f"{output_prefix}.bin"
        print(f"写入 SPTAG DEFAULT 格式: {output_file}")
        write_sptag_default(output_file, vectors, k)
    elif format_type == 'xvec':
        output_file = f"{output_prefix}.xvec"
        print(f"写入 SPTAG XVEC 格式: {output_file}")
        write_sptag_xvec(output_file, vectors, k)
    elif format_type == 'txt':
        output_file = f"{output_prefix}.txt"
        print(f"写入 SPTAG TXT 格式: {output_file}")
        write_sptag_txt(output_file, vectors, k)
    else:
        print(f"未知格式: {format_type}")
        sys.exit(1)

    print("转换完成！")

if __name__ == '__main__':
    main()
