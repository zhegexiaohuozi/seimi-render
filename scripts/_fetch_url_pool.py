#!/usr/bin/env python3
"""一次性：用 seimi-render 渲染媒体站点首页，提取文章链接组成压测 URL 池。
输出 Python 字面量到 stdout，便于直接内嵌进 soak_test.py。"""
import json
import re
import sys
import urllib.request

BASE = "http://127.0.0.1:8088"

# (站点名, 首页 URL, 提取文章链接的正则)
# 每个站点的文章 URL 形态不同，分别匹配：
#   搜狐:  //www.sohu.com/a/<id>_<seq>
#   网易:  //www.163.com/<...>/article/<id>.html
#   新浪:  //(<sub>)?.sina.com.cn/.../<date>/<id>.shtml
#   澎湃:  //www.thepaper.cn/newsDetail_forward<id>
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
    """渲染首页，失败重试。返回 HTML 或空串。"""
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
    """从 HTML 提取文章链接。匹配结果可能是完整路径(//host/...) 或裸 id，
    后者拼回 https://www.thepaper.cn/newsDetail_forward_<id>。"""
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

    # 输出可直接粘贴的字面量
    print("URL_POOL = {")
    for name, links in pool.items():
        print(f"    {name!r}: [")
        for u in links:
            print(f"        {u!r},")
        print("    ],")
    print("}")


if __name__ == "__main__":
    main()
