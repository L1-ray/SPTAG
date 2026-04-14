#!/usr/bin/env python3
"""
SPTAG 在线模式性能测试脚本

在线模式特点：
- 构建和搜索在同一进程中完成
- 无需保存索引到磁盘
- 适合实时构建、临时使用场景

对比离线模式：
- 离线模式：Build → Save → Load → Search
- 在线模式：Build → Search（同一进程）
"""

import SPTAG
import numpy as np
import time
import os
import sys
import argparse
from pathlib import Path


def load_fvecs(filepath):
    """加载 fvecs 格式文件"""
    with open(filepath, 'rb') as f:
        dim = np.frombuffer(f.read(4), dtype=np.int32)[0]
        f.seek(0, 2)
        size = f.tell()
        n = size // (4 + dim * 4)
        f.seek(0)
        data = np.frombuffer(f.read(), dtype=np.float32)
        return data.reshape(n, dim + 1)[:, 1:].copy()


def load_ivecs(filepath):
    """加载 ivecs 格式文件（ground truth）"""
    with open(filepath, 'rb') as f:
        data = []
        while True:
            dim_bytes = f.read(4)
            if not dim_bytes:
                break
            dim = np.frombuffer(dim_bytes, dtype=np.int32)[0]
            vec = np.frombuffer(f.read(dim * 4), dtype=np.int32)
            data.append(vec)
        return np.array(data)


def calculate_recall(results, ground_truth, k):
    """计算召回率"""
    correct = 0
    total = len(results)
    for i, res in enumerate(results):
        gt_set = set(ground_truth[i][:k])
        res_set = set(res[:k])
        correct += len(gt_set & res_set)
    return correct / (total * k)


def main():
    parser = argparse.ArgumentParser(description='SPTAG 在线模式性能测试')
    parser.add_argument('--base', type=str,
                        default='/media/ray/1tb/sift1m/sift_base.fvecs',
                        help='基础向量文件路径')
    parser.add_argument('--query', type=str,
                        default='/media/ray/1tb/sift1m/sift_query.fvecs',
                        help='查询向量文件路径')
    parser.add_argument('--truth', type=str,
                        default='/media/ray/1tb/sift1m/sift_groundtruth.ivecs',
                        help='Ground truth 文件路径')
    parser.add_argument('--threads', type=int, default=8,
                        help='构建线程数')
    parser.add_argument('--k', type=int, default=32,
                        help='返回的最近邻数量')
    parser.add_argument('--maxcheck', type=int, default=8192,
                        help='搜索时最大检查节点数')
    parser.add_argument('--output', type=str, default=None,
                        help='可选：保存索引到指定目录')
    parser.add_argument('--no-search', action='store_true',
                        help='仅构建索引，不执行搜索')
    parser.add_argument('--batch', action='store_true',
                        help='使用批量搜索模式（性能更高）')
    args = parser.parse_args()

    print("=" * 50)
    print("SPTAG 在线模式性能测试")
    print("=" * 50)

    # 1. 加载数据
    print("\n[1] 加载数据...")
    print(f"    基础向量: {args.base}")
    print(f"    查询向量: {args.query}")

    start_load = time.time()
    base_vectors = load_fvecs(args.base)
    query_vectors = load_fvecs(args.query)
    ground_truth = load_ivecs(args.truth)
    load_time = time.time() - start_load

    print(f"    基础向量: {base_vectors.shape}")
    print(f"    查询向量: {query_vectors.shape}")
    print(f"    加载耗时: {load_time:.2f}s")

    # 2. 构建索引（在线模式核心）
    print(f"\n[2] 构建索引（在线模式，{args.threads} 线程）...")

    start_build = time.time()

    # 创建索引
    index = SPTAG.AnnIndex('BKT', 'Float', base_vectors.shape[1])

    # 设置构建参数
    index.SetBuildParam("DistCalcMethod", "L2", "Index")
    index.SetBuildParam("NumberOfThreads", str(args.threads), "Index")
    index.SetBuildParam("NeighborhoodSize", "32", "Index")
    index.SetBuildParam("TPTNumber", "8", "Index")
    index.SetBuildParam("RefineIterations", "1", "Index")

    # 构建索引
    success = index.Build(base_vectors, base_vectors.shape[0], False)

    build_time = time.time() - start_build

    if not success:
        print("    错误：索引构建失败！")
        sys.exit(1)

    print(f"    构建耗时: {build_time:.2f}s ({build_time/60:.2f}分钟)")
    print(f"    构建速度: {base_vectors.shape[0]/build_time:.0f} vectors/s")

    # 3. 设置搜索参数（可选，索引有默认值）
    print(f"\n[3] 设置搜索参数 (MaxCheck={args.maxcheck})...")
    try:
        index.SetSearchParam("MaxCheck", str(args.maxcheck), "Index")
        print("    参数设置成功")
    except Exception as e:
        print(f"    警告：参数设置失败: {e}")
        print("    将使用默认参数继续")

    # 4. 搜索测试
    if not args.no_search:
        if args.batch:
            # 批量搜索模式（性能更高）
            print(f"\n[4] 批量搜索测试 (K={args.k}, Batch模式)...")

            start_search = time.time()

            # BatchSearch 返回 (vecIDs, vecDists, metadata, relaxMono)
            # vecIDs 是所有查询结果拼接的列表
            batch_result = index.BatchSearch(query_vectors.tobytes(), len(query_vectors), args.k, False)
            all_vec_ids = batch_result[0]

            search_time = time.time() - start_search
            qps = len(query_vectors) / search_time

            # 解析批量结果
            results = []
            for i in range(len(query_vectors)):
                start_idx = i * args.k
                end_idx = start_idx + args.k
                results.append(all_vec_ids[start_idx:end_idx])

            recall = calculate_recall(results, ground_truth, args.k)

            print(f"\n[搜索结果]")
            print(f"    总查询数: {len(query_vectors)}")
            print(f"    搜索耗时: {search_time:.3f}s")
            print(f"    QPS: {qps:.2f}")
            print(f"    Recall@{args.k}: {recall*100:.2f}%")
        else:
            # 单个搜索模式
            print(f"\n[4] 搜索测试 (K={args.k}, 单个模式)...")

            results = []
            start_search = time.time()

            for i, query in enumerate(query_vectors):
                # Search 返回 (vecIDs, vecDists, metadata, relaxMono) 元组
                result = index.Search(query, args.k)
                vec_ids = result[0]  # 第一个元素是向量 ID 列表
                results.append(vec_ids)

                if (i + 1) % 1000 == 0:
                    elapsed = time.time() - start_search
                    qps = (i + 1) / elapsed
                    print(f"    已处理 {i+1}/{len(query_vectors)} 查询, QPS: {qps:.1f}")

            search_time = time.time() - start_search
            qps = len(query_vectors) / search_time

            # 计算召回率
            recall = calculate_recall(results, ground_truth, args.k)

            print(f"\n[搜索结果]")
            print(f"    总查询数: {len(query_vectors)}")
            print(f"    搜索耗时: {search_time:.3f}s")
            print(f"    QPS: {qps:.2f}")
            print(f"    Recall@{args.k}: {recall*100:.2f}%")

    # 5. 可选：保存索引
    if args.output:
        print(f"\n[4] 保存索引到: {args.output}")
        os.makedirs(args.output, exist_ok=True)
        index.Save(args.output)
        print("    保存完成")

    # 6. 总结
    print("\n" + "=" * 50)
    print("测试总结")
    print("=" * 50)
    print(f"模式: 在线模式")
    print(f"基础向量: {base_vectors.shape[0]:,}")
    print(f"查询向量: {query_vectors.shape[0]:,}")
    print(f"构建线程: {args.threads}")
    print(f"构建耗时: {build_time:.2f}s")
    if not args.no_search:
        print(f"搜索QPS: {qps:.2f}")
        print(f"Recall@{args.k}: {recall*100:.2f}%")
    print("=" * 50)


if __name__ == '__main__':
    main()
