import argparse
import asyncio
import time
from collections import Counter

import aiohttp


# 核心请求函数
async def fetch(semaphore, session, url):
    async with semaphore:
        start_time = time.perf_counter()
        try:
            # 发起 GET 请求
            async with session.get(url) as response:
                # 读取完整响应体（确保网络 I/O 真正完成）
                await response.read()
                latency = time.perf_counter() - start_time
                return response.status, latency
        except Exception as e:
            latency = time.perf_counter() - start_time
            return f"Error: {str(e)}", latency


# 主控测试函数
async def run_load_test(url, total_requests, concurrency):
    print(f"🚀 开始测试目标: {url}")
    print(f"📦 总请求数: {total_requests} | 🔀 并发数: {concurrency} ...\n")

    # 控制并发量的信号量
    semaphore = asyncio.Semaphore(concurrency)

    # 优化 TCP 连接池设置，配合高并发
    connector = aiohttp.TCPConnector(limit=concurrency)

    async with aiohttp.ClientSession(connector=connector) as session:
        # 创建所有任务
        tasks = [fetch(semaphore, session, url) for _ in range(total_requests)]

        # 记录总耗时
        test_start = time.perf_counter()
        results = await asyncio.gather(*tasks)
        test_end = time.perf_counter()

    # --- 统计与分析数据 ---
    total_time = test_end - test_start
    statuses = [res[0] for res in results]
    latencies = [res[1] * 1000 for res in results]  # 转换为毫秒

    status_counts = Counter(statuses)
    avg_latency = sum(latencies) / len(latencies)
    max_latency = max(latencies)
    min_latency = min(latencies)
    rps = total_requests / total_time

    # 打印测试报告
    print("-" * 40)
    print("📊 测试结果报告")
    print("-" * 40)
    print(f"⏱️  总耗时:       {total_time:.3f} 秒")
    print(f"⚡ RPS (吞吐量): {rps:.2f} 请求/秒")
    print(f"📉 最小延迟:     {min_latency:.2f} ms")
    print(f"📈 最大延迟:     {max_latency:.2f} ms")
    print(f"⚖️  平均延迟:     {avg_latency:.2f} ms")
    print("-" * 40)
    print("🔢 状态码分布:")
    for status, count in status_counts.items():
        print(f"   [{status}]: {count} 次")
    print("-" * 40)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="API 并发压测工具")
    parser.add_argument(
        "--host", type=str, default="http://127.0.0.1:8000", help="服务器基础地址"
    )
    parser.add_argument(
        "--endpoint",
        type=str,
        default="/prearms",
        help="测试的具体路由 (例如 /prearms 或 /not_exist_404)",
    )
    parser.add_argument("-n", "--requests", type=int, default=1000, help="总请求数")
    parser.add_argument("-c", "--concurrency", type=int, default=50, help="并发数量")

    args = parser.parse_args()

    target_url = f"{args.host.rstrip('/')}/{args.endpoint.lstrip('/')}"
    print(target_url)
    # 解决 Windows 平台下的 asyncio 报错问题
    if hasattr(asyncio, "WindowsSelectorEventLoopPolicy"):
        asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())

    asyncio.run(run_load_test(target_url, args.requests, args.concurrency))
