#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 wanghaomiao.cn
# SPDX-License-Identifier: Apache-2.0
#
"""一次性：渲染媒体站点首页，提取文章链接组成压测 URL 池，输出 Python 字面量。"""
import json
import re
import sys
import urllib.request

BASE = "http://127.0.0.1:8088"

# (站点名, 首页 URL, 文章链接正则)
SITES = [
    ("sohu",  "https://www.sohu.com/",
     r'//www\.sohu\.com/a/\d+_\d+'),
    ("163",   "https://www.163.com/",
     r'//www\.163\.com/[a-z]+/article/[A-Z0-9]+\.html'),
    ("sina",  "https://www.sina.com.cn/",
     r'//news\.sina\.com\.cn/[a-z]+/\d{4}-\d{2}-\d{2}/doc-[a-z0-9]+\.shtml|//(?:finance|tech|sports|ent)\.sina\.com\.cn/[a-z]+/\d{4}-\d{2}-\d{2}/doc-[a-z0-9]+\.shtml'),
    ("thepaper", "https://www.thepaper.cn/",
     r'//www\.thepaper\.cn/newsDetail_forward_\d+|newsDetail_forward_\d+'),
]

PATTERN_HTTPS = re.compile(r'^https?://', re.I)


def render(url, retries=2):
    for attempt in range(retries + 1):
        body = json.dumps({"url": url, "settle_ms": 4500,
                           "long_poll_ms": 40000, "output": "html"}).encode()
        req = urllib.request.Request(BASE + "/render", data=body,
                                     headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=50) as r:
                d = json.loads(r.read())
        except Exception as e:  # noqa: BLE001
            sys.stderr.write(f"  attempt {attempt+1} error: {e}\n")
            continue
        if d.get("state") == "succeeded":
            return d.get("html", "")
        sys.stderr.write(f"  attempt {attempt+1} -> {d.get('state')} {d.get('error','')}\n")
    return ""


def extract(html, pat):
    """提取文章链接；thepaper 的裸 id 拼回完整 URL。"""
    seen = []
    seen_set = set()
    for m in re.findall(pat, html):
        if m.startswith("//"):
            u = "https:" + m
        elif m.startswith("newsDetail_forward_"):
            u = "https://www.thepaper.cn/" + m
        else:
            continue
        if u in seen_set:
            continue
        seen_set.add(u)
        seen.append(u)
        if len(seen) >= 20:
            break
    return seen


def main():
    pool = {}
    for name, home, pat in SITES:
        sys.stderr.write(f"rendering {name}: {home}\n")
        html = render(home)
        if not html:
            sys.stderr.write(f"  WARN: {name} no html, skipping\n")
            continue
        links = extract(html, pat)
        sys.stderr.write(f"  got {len(links)} links\n")
        if len(links) < 20:
            sys.stderr.write(f"  WARN: {name} only {len(links)} (<20)\n")
        pool[name] = links

    print("URL_POOL = {")
    for name, links in pool.items():
        print(f"    {name!r}: [")
        for u in links:
            print(f"        {u!r},")
        print("    ],")
    print("}")


if __name__ == "__main__":
    main()
