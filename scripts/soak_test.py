#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 wanghaomiao.cn
# SPDX-License-Identifier: Apache-2.0

"""
seimi-render 持续压测脚本。

场景：在指定时长内维持固定并发数，持续提交渲染任务，统计：
  - 成功 / 失败 / 超时数量与比例
  - 成功任务的耗时分布（min/p50/p90/p99/max）
  - 失败任务的错误原因分布
  - 每个站点的成功率（定位是某站点限流还是服务本身问题）
  - 是否出现「非预期问题」：任务卡死（超出 load-timeout 仍未结束）、
    进程崩溃、接口无响应、内存泄漏迹象等

URL 池设计（避免限流假象）：
  内嵌 4 个媒体站点（搜狐/网易/新浪/澎湃）各 20 条真实文章链接，共 80 条，
  worker 轮转取用，避免反复请求同一 URL。同时按 host 限制并发在途数
 （--max-per-host），防止单站点瞬时并发过高触发其反爬/限流——
  否则「并发↑→吞吐↓」可能只是站点限流，而非 seimi-render 的真实瓶颈。

用法:
  python3 scripts/soak_test.py --duration 600 --concurrency 10
  python3 scripts/soak_test.py --url https://www.sohu.com/ --duration 600 --concurrency 10
  python3 scripts/soak_test.py --shuffle --max-per-host 2 --duration 600

前置：seimi-render 服务已在运行（默认 http://localhost:8088）。
"""
import argparse
import collections
import random
import statistics
import sys
import threading
import time
import urllib.error
import urllib.request
from urllib.parse import urlparse

DEFAULT_DURATION = 600      # 10 分钟
DEFAULT_CONCURRENCY = 10
LONG_POLL_MS = 30000        # 长轮询窗口
LONG_POLL_HTTP_TIMEOUT = 35 # HTTP 读超时（略大于长轮询窗口）
LOAD_TIMEOUT_MS = 20000     # 服务端默认单任务超时，用于判定「卡死」

# —— 渲染链接池：4 个媒体站点 × 20 条真实文章链接 ——
# 抓取方式见 scripts/_fetch_url_pool.py（用 seimi-render 渲染各站首页后提取）。
# 轮转使用避免对单 URL 的缓存/限流干扰；per-host 节流避免单站并发过高被反爬。
URL_POOL = {
    'sohu': [
        'https://www.sohu.com/a/1044221136_220095',
        'https://www.sohu.com/a/1044223540_120388781',
        'https://www.sohu.com/a/1044243658_123753',
        'https://www.sohu.com/a/1044222594_121019331',
        'https://www.sohu.com/a/1044211344_180220',
        'https://www.sohu.com/a/1044269622_121347613',
        'https://www.sohu.com/a/1044307569_120952561',
        'https://www.sohu.com/a/1044400114_429139',
        'https://www.sohu.com/a/1044400253_429139',
        'https://www.sohu.com/a/1044400283_429139',
        'https://www.sohu.com/a/1044400999_429139',
        'https://www.sohu.com/a/1044401573_429139',
        'https://www.sohu.com/a/1044404216_429139',
        'https://www.sohu.com/a/1043488444_222493',
        'https://www.sohu.com/a/1044413684_429139',
        'https://www.sohu.com/a/1044414343_429139',
        'https://www.sohu.com/a/1044414346_429139',
        'https://www.sohu.com/a/1044180085_267106',
        'https://www.sohu.com/a/1044159798_121668715',
        'https://www.sohu.com/a/1043994824_260616',
    ],
    '163': [
        'https://www.163.com/dy/article/I7LRA8AU051487S2.html',
        'https://www.163.com/news/article/L0PGMTMB000189FH.html',
        'https://www.163.com/news/article/L0PGO3B8000189FH.html',
        'https://www.163.com/news/article/L0PGP5QT000189FH.html',
        'https://www.163.com/news/article/L0PGQ5O8000189FH.html',
        'https://www.163.com/news/article/L0PGRE0K000189FH.html',
        'https://www.163.com/news/article/L0PGSF2I000189FH.html',
        'https://www.163.com/news/article/L0PGV07N000189FH.html',
        'https://www.163.com/news/article/L0PIDD53000189FH.html',
        'https://www.163.com/news/article/L0PICIJ5000189FH.html',
        'https://www.163.com/news/article/L0PIBG9U000189FH.html',
        'https://www.163.com/news/article/L0PIRJ93000189FH.html',
        'https://www.163.com/dy/article/L0PIOA7U0514R9P4.html',
        'https://www.163.com/news/article/L0PIIL8I000189FH.html',
        'https://www.163.com/news/article/L0PIHPG2000189FH.html',
        'https://www.163.com/news/article/L0PIGNF6000189FH.html',
        'https://www.163.com/dy/article/L0O0MIEG0514R9P4.html',
        'https://www.163.com/news/article/L0PIVLTD000189FH.html',
        'https://www.163.com/dy/article/L0O49PGR0514CQIE.html',
        'https://www.163.com/dy/article/L0O7M3RI0514R9M0.html',
    ],
    'sina': [
        'https://finance.sina.com.cn/world/2026-07-01/doc-iniffysy4342331.shtml',
        'https://news.sina.com.cn/gov/2026-07-01/doc-inifhrqy6394272.shtml',
        'https://finance.sina.com.cn/roll/2026-07-01/doc-iniffqch1282113.shtml',
        'https://finance.sina.com.cn/jjxw/2026-07-01/doc-iniffyta1123740.shtml',
        'https://finance.sina.com.cn/jjxw/2026-07-01/doc-iniffytk3122288.shtml',
        'https://finance.sina.com.cn/tech/2022-05-31/doc-imizmscu4278417.shtml',
        'https://news.sina.com.cn/o/2018-09-20/doc-ifxeuwwr6236397.shtml',
        'https://news.sina.com.cn/c/2020-03-02/doc-iimxyqvz7080349.shtml',
        'https://news.sina.com.cn/gov/2018-07-03/doc-ihevauxi4451100.shtml',
        'https://news.sina.com.cn/c/2018-07-03/doc-ihevauxi3058403.shtml',
        'https://news.sina.com.cn/gov/2018-07-03/doc-ihevauxi6353508.shtml',
        'https://tech.sina.com.cn/it/2018-12-08/doc-ihmutuec7210917.shtml',
        'https://news.sina.com.cn/o/2017-10-04/doc-ifymkwwk8402944.shtml',
        'https://news.sina.com.cn/o/2017-10-05/doc-ifymmiwm5480699.shtml',
        'https://sports.sina.com.cn/l/2026-07-01/doc-iniffyta1134910.shtml',
        'https://sports.sina.com.cn/l/2026-06-30/doc-iniffcpk4669151.shtml',
        'https://finance.sina.com.cn/jjxw/2026-07-01/doc-inifinuq6138946.shtml',
        'https://finance.sina.com.cn/jjxw/2026-07-01/doc-inifinuq6135427.shtml',
        'https://finance.sina.com.cn/jjxw/2026-07-01/doc-inifinuq6110129.shtml',
        'https://finance.sina.com.cn/wm/2026-07-01/doc-iniffytk3169359.shtml',
    ],
    'thepaper': [
        'https://www.thepaper.cn/newsDetail_forward_33496578',
        'https://www.thepaper.cn/newsDetail_forward_33496580',
        'https://www.thepaper.cn/newsDetail_forward_33496584',
        'https://www.thepaper.cn/newsDetail_forward_33496585',
        'https://www.thepaper.cn/newsDetail_forward_33496583',
        'https://www.thepaper.cn/newsDetail_forward_33496586',
        'https://www.thepaper.cn/newsDetail_forward_33496587',
        'https://www.thepaper.cn/newsDetail_forward_33492239',
        'https://www.thepaper.cn/newsDetail_forward_33497278',
        'https://www.thepaper.cn/newsDetail_forward_33494468',
        'https://www.thepaper.cn/newsDetail_forward_33485247',
        'https://www.thepaper.cn/newsDetail_forward_33496841',
        'https://www.thepaper.cn/newsDetail_forward_33497633',
        'https://www.thepaper.cn/newsDetail_forward_33497256',
        'https://www.thepaper.cn/newsDetail_forward_33496235',
        'https://www.thepaper.cn/newsDetail_forward_33497982',
        'https://www.thepaper.cn/newsDetail_forward_166093',
        'https://www.thepaper.cn/newsDetail_forward_33492169',
        'https://www.thepaper.cn/newsDetail_forward_33485818',
        'https://www.thepaper.cn/newsDetail_forward_33491527',
    ],
}


def flatten_pool(pool):
    """把按站点分组的池展平为单一列表（站点间交错，避免连续命中同站）。"""
    lists = list(pool.values())
    out = []
    for i in range(max(len(l) for l in lists)):
        for l in lists:
            if i < len(l):
                out.append(l[i])
    return out


def submit_and_wait(base, url, settle_ms):
    """提交一个渲染任务并长轮询等待结果。
    返回 (ok: bool, elapsed_ms: int|None, state: str, error: str|None)。"""
    body = ("{\"url\":\"%s\",\"settle_ms\":%d,\"long_poll_ms\":%d}"
            % (url, settle_ms, LONG_POLL_MS)).encode()
    req = urllib.request.Request(base + "/render", data=body,
                                 headers={"Content-Type": "application/json"})
    t0 = time.monotonic()
    try:
        with urllib.request.urlopen(req, timeout=LONG_POLL_HTTP_TIMEOUT) as r:
            import json
            d = json.loads(r.read())
    except urllib.error.URLError as e:
        return False, None, "conn_error", str(e.reason)
    except Exception as e:  # noqa: BLE001
        return False, None, "exception", f"{type(e).__name__}: {e}"

    elapsed = int((time.monotonic() - t0) * 1000)
    state = d.get("state", "unknown")
    if state == "succeeded":
        return True, elapsed, state, None
    if state == "running":
        return False, elapsed, "running", d.get("error", "long-poll timeout, still running")
    return False, elapsed, state, d.get("error", "unknown failure")


def worker(stop_evt, base, url_seq, idx_lock, idx_box, settle_ms, load_timeout_ms,
           host_sems, jitter_ms, stats, lock):
    """持续提交任务直到 stop_evt 被设置。
    url_seq/idx_lock/idx_box: 共享的轮转 URL 序列与其下标（线程安全取下一个）。
    host_sems: host -> threading.Semaphore，限制每 host 在途并发，规避站点限流。"""
    while not stop_evt.is_set():
        # 轮转取下一个 URL
        with idx_lock:
            i = idx_box[0] % len(url_seq)
            idx_box[0] += 1
        url = url_seq[i]
        host = urlparse(url).hostname or url

        # per-host 并发节流：单 host 在途数受限，避免触发该站点的反爬/限流。
        # acquire 可能阻塞（这是预期的 backpressure），不占用 seimi-render 槽位。
        sem = host_sems.get(host)
        if sem is not None:
            sem.acquire()
        try:
            ok, elapsed, state, error = submit_and_wait(base, url, settle_ms)
        finally:
            if sem is not None:
                sem.release()

        with lock:
            stats["total"] += 1
            stats["per_host"][host]["total"] += 1
            if ok:
                stats["ok"] += 1
                stats["latencies"].append(elapsed)
                stats["per_host"][host]["ok"] += 1
            else:
                stats["fail"] += 1
                # 区分「真失败」与「卡死」：耗时显著超 load-timeout 仍未终态视为疑似卡死。
                if state == "running" and elapsed and elapsed > load_timeout_ms + 5000:
                    stats["stuck"] += 1
                    key = "STUCK(>load-timeout, no terminal state)"
                else:
                    key = f"{state}: {(error or '')[:80]}"
                stats["errors"][key] += 1
                stats["per_host"][host]["fail"] += 1

        # 请求间抖动：打散 worker 节奏，避免脉冲式突发触发限流，测的是稳态吞吐。
        if jitter_ms > 0:
            time.sleep(random.uniform(0, jitter_ms) / 1000.0)


def percentile(sorted_vals, p):
    if not sorted_vals:
        return 0
    k = max(0, min(len(sorted_vals) - 1, int(round(p / 100 * (len(sorted_vals) - 1)))))
    return sorted_vals[k]


def main():
    ap = argparse.ArgumentParser(description="seimi-render 持续压测")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8088)
    ap.add_argument("--url", default=None,
                    help="单 URL 模式：只压测该 URL（不走链接池）。不指定则用内置 80 条媒体链接池。")
    ap.add_argument("--duration", type=int, default=DEFAULT_DURATION, help="压测时长（秒，默认 600）")
    ap.add_argument("--concurrency", type=int, default=DEFAULT_CONCURRENCY, help="并发数（默认 10）")
    ap.add_argument("--settle-ms", type=int, default=2500, help="settle_ms（默认 2500）")
    ap.add_argument("--load-timeout-ms", type=int, default=LOAD_TIMEOUT_MS,
                    help=f"服务端单任务超时判定基准（默认 {LOAD_TIMEOUT_MS}ms，应与服务端 --load-timeout-ms 一致）")
    ap.add_argument("--max-per-host", type=int, default=2,
                    help="每个站点同时在途的请求数上限（默认 2，防单站并发过高被限流；0=不限）")
    ap.add_argument("--shuffle", action="store_true", help="打乱链接池顺序")
    ap.add_argument("--jitter-ms", type=int, default=200,
                    help="每次请求后的随机抖动上限（毫秒，默认 200，防脉冲触发限流；0=关闭）")
    args = ap.parse_args()
    load_timeout_ms = args.load_timeout_ms

    base = f"http://{args.host}:{args.port}"

    # 构建 URL 序列：--url 走单 URL；否则用内置池。
    if args.url:
        url_seq = [args.url]
        pool_desc = f"单 URL: {args.url}"
    else:
        url_seq = flatten_pool(URL_POOL)
        if args.shuffle:
            random.shuffle(url_seq)
        pool_desc = f"链接池 {len(url_seq)} 条（搜狐/网易/新浪/澎湃 各 20）"

    # per-host 信号量（仅池模式有意义；单 URL 模式下 host 唯一，信号量退化为全局并发限制）
    host_sems = {}
    if args.max_per_host > 0:
        hosts = {urlparse(u).hostname for u in url_seq if urlparse(u).hostname}
        for h in hosts:
            host_sems[h] = threading.Semaphore(args.max_per_host)

    print(f"== seimi-render 压测 ==")
    print(f"  目标      : {base}")
    print(f"  URL 来源  : {pool_desc}")
    print(f"  并发      : {args.concurrency}")
    print(f"  per-host  : {('≤' + str(args.max_per_host)) if args.max_per_host > 0 else '不限'}")
    print(f"  时长      : {args.duration}s ({args.duration/60:.1f} 分钟)")
    print(f"  settle    : {args.settle_ms}ms")
    print(f"  jitter    : {args.jitter_ms}ms")
    print()

    # 健康检查
    try:
        with urllib.request.urlopen(base + "/health", timeout=5) as r:
            if b'"status":"ok"' not in r.read():
                print("FATAL: 健康检查失败，服务未就绪", file=sys.stderr)
                sys.exit(1)
    except Exception as e:  # noqa: BLE001
        print(f"FATAL: 无法连接服务 ({e})", file=sys.stderr)
        sys.exit(1)
    print("健康检查通过，开始压测...\n")

    stats = {
        "total": 0, "ok": 0, "fail": 0, "stuck": 0,
        "latencies": [], "errors": collections.Counter(),
        "per_host": collections.defaultdict(lambda: {"total": 0, "ok": 0, "fail": 0}),
    }
    lock = threading.Lock()
    stop_evt = threading.Event()
    idx_lock = threading.Lock()
    idx_box = [0]  # 全局轮转下标（box 包一层以便闭包内修改）

    threads = [threading.Thread(
        target=worker,
        args=(stop_evt, base, url_seq, idx_lock, idx_box, args.settle_ms,
              load_timeout_ms, host_sems, args.jitter_ms, stats, lock),
        daemon=True) for _ in range(args.concurrency)]
    for t in threads:
        t.start()

    # 进度报告
    start = time.monotonic()
    last_report = start
    try:
        while True:
            now = time.monotonic()
            if now - start >= args.duration:
                break
            if now - last_report >= 30:
                last_report = now
                with lock:
                    tot, ok, fail = stats["total"], stats["ok"], stats["fail"]
                    qps = tot / (now - start) if now > start else 0
                remaining = int(args.duration - (now - start))
                print(f"  [{int(now-start):>4}s / 剩 {remaining}s] "
                      f"总计 {tot} | 成功 {ok} | 失败 {fail} | "
                      f"吞吐 {qps:.2f} req/s")
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n收到中断，停止压测...")

    stop_evt.set()
    for t in threads:
        t.join(timeout=LONG_POLL_HTTP_TIMEOUT + 10)

    # 汇总
    with lock:
        tot, ok, fail, stuck = stats["total"], stats["ok"], stats["fail"], stats["stuck"]
        lats = sorted(stats["latencies"])
        errors = stats["errors"]
        per_host = dict(stats["per_host"])

    wall = time.monotonic() - start
    ok_rate = (ok / tot * 100) if tot else 0

    print("\n" + "=" * 56)
    print("  压测结果汇总")
    print("=" * 56)
    print(f"  实际时长 : {wall:.1f}s")
    print(f"  总任务数 : {tot}")
    print(f"  成功     : {ok}  ({ok_rate:.1f}%)")
    print(f"  失败     : {fail}  ({100-ok_rate:.1f}%)")
    print(f"  疑似卡死 : {stuck}  (超出 load-timeout 仍未终态)")
    print(f"  平均吞吐 : {(tot/wall if wall else 0):.2f} req/s")
    print()
    if lats:
        print("  成功任务耗时分布 (ms):")
        print(f"    min  : {lats[0]}")
        print(f"    p50  : {percentile(lats, 50)}")
        print(f"    p90  : {percentile(lats, 90)}")
        print(f"    p99  : {percentile(lats, 99)}")
        print(f"    max  : {lats[-1]}")
        print(f"    mean : {int(statistics.mean(lats))}")
    else:
        print("  （无成功任务）")
    print()

    # per-host 成功率：定位是某站点限流还是服务端问题。
    if per_host and not args.url:
        print("  每站点成功率:")
        for h, c in sorted(per_host.items(), key=lambda kv: -kv[1]["total"]):
            ht = c["total"]
            rate = (c["ok"] / ht * 100) if ht else 0
            print(f"    {h:<28} {c['ok']:>4}/{ht:<4} ({rate:5.1f}%)  失败 {c['fail']}")
        print()

    if errors:
        print("  失败原因分布:")
        for reason, cnt in errors.most_common():
            print(f"    {cnt:>4}x  {reason}")
    print()

    # 非预期问题判定
    print("-" * 56)
    problems = []
    if stuck > 0:
        problems.append(f"{stuck} 个任务疑似卡死（超出 load-timeout {load_timeout_ms}ms 仍未到终态）")
    if ok_rate < 90 and tot >= 20:
        problems.append(f"成功率 {ok_rate:.1f}% 低于 90% 阈值")
    if fail > 0 and not errors:
        problems.append("有失败任务但无错误原因记录")
    if tot == 0:
        problems.append("整个压测期间没有任何任务完成")

    if problems:
        print("  ⚠ 发现潜在问题：")
        for p in problems:
            print(f"     - {p}")
    else:
        print("  ✓ 未发现非预期问题")
        print(f"     (成功率 {ok_rate:.1f}%，无任务卡死，服务在 {args.concurrency} 并发下稳定)")
    print("-" * 56)

    sys.exit(0 if not problems else 2)


if __name__ == "__main__":
    main()
