#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 wanghaomiao.cn
# SPDX-License-Identifier: Apache-2.0
"""Exercise Markdown through the real HTTP/WS/MCP server, using local fixtures.

Requires requests and websocket-client. Builds are intentionally separate:
    python scripts/test_markdown_api.py --binary build/seimi-render.exe
All ports and persistent data are isolated; Chromium subprocesses are cleaned up.
"""

import argparse
from contextlib import closing, contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

import requests
import websocket


ROOT = Path(__file__).resolve().parents[1]
HOST = "127.0.0.1"
UNICODE_TEXT = "BEGIN " + ("汉字😀é𝄞 fidelity " * 9000) + "END"
ARTICLE_TEXT = (
    "Reliable article extraction preserves meaningful sentences and their context. "
    "A browser has already parsed the document, so the conversion can operate on "
    "real elements while preserving the original page for other output formats. "
    "This paragraph supplies enough substantive prose for deterministic article detection. "
)
FIXTURES = {
    "/basic": """<!doctype html><html><head><meta charset="utf-8"><title>Fixture</title>
      <style>body{font:18px sans-serif;background:#fff;color:#111}.hidden{display:none}</style>
      </head><body><h1>Fixture heading</h1><p>Visible content</p>
      <p class="hidden">HIDDEN SENTINEL</p><p aria-hidden="true">Visible aria sentinel</p>
      <a data-href="/tracking" href="/real">Exact link</a>
      <ul><li><input type="checkbox" checked>Finished task</li></ul>
      <table><tr><th>Name</th><th>Value</th></tr><tr><td>Alpha</td><td>42</td></tr></table>
      <pre><code class="language-js">const x = 1;\nconsole.log(x);</code></pre>
      <script>window.TurndownService=function(){throw Error('page-world collision')};
      window.__seimiMarkdownOptions={converter:'invalid'};</script>
      </body></html>""",
    "/article": "<!doctype html><html><head><meta charset='utf-8'><title>Article title</title></head>"
      "<body><nav><a href='/menu'>NAVIGATION SENTINEL</a></nav><article><h1>Article title</h1>"
      + "".join(f"<p>Paragraph {i}. {ARTICLE_TEXT * 3}</p>" for i in range(6))
      + "</article><footer>FOOTER SENTINEL</footer></body></html>",
    "/short": "<!doctype html><html><head><title></title></head><body><p>Tiny body.</p></body></html>",
    "/empty": "<!doctype html><html><head><title></title></head><body></body></html>",
    "/delayed": "<!doctype html><html><head><title></title></head><body><p id='state'>BEFORE</p>"
      "<script>setTimeout(function(){document.getElementById('state').textContent='AFTER'},1000);</script></body></html>",
    "/unicode": "<!doctype html><html><head><meta charset='utf-8'><title></title></head>"
      "<body><p>" + UNICODE_TEXT + "</p></body></html>",
}


def check(condition, message):
    if not condition:
        raise AssertionError(message)


class FixtureHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        body = FIXTURES.get(self.path)
        self.send_response(200 if body is not None else 404)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        data = (body or "Not found").encode("utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *_args):
        pass


def stop_process(proc):
    if proc is None:
        return
    if os.name == "nt":
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       creationflags=subprocess.CREATE_NO_WINDOW, check=False)
    else:
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        if os.name == "nt":
            proc.kill()
        else:
            os.killpg(proc.pid, signal.SIGKILL)
        proc.wait(timeout=5)


@contextmanager
def running_server(binary, qt_bin, concurrency=2, load_timeout_ms=30000):
    fixture = ThreadingHTTPServer((HOST, 0), FixtureHandler)
    fixture_thread = threading.Thread(target=fixture.serve_forever, daemon=True)
    fixture_thread.start()
    ports = []
    reservations = []
    for _ in range(3):
        reservation = socket.socket()
        reservation.bind((HOST, 0))
        ports.append(reservation.getsockname()[1])
        reservations.append(reservation)
    proc = None
    session = requests.Session()
    session.trust_env = False
    try:
        with tempfile.TemporaryDirectory(prefix="seimi-markdown-api-") as data_dir:
            log_path = Path(data_dir) / "server.log"
            env = os.environ.copy()
            if qt_bin:
                env["PATH"] = str(qt_bin) + os.pathsep + env.get("PATH", "")
            env["QT_QPA_PLATFORM"] = "offscreen"
            args = [str(binary), "--host", HOST, "--http-port", str(ports[0]),
                    "--ws-port", str(ports[1]), "--mcp-port", str(ports[2]),
                    "--data-dir", data_dir, "--no-warmup", "--no-stealth", "--no-ssrf",
                    "--no-admin", "--concurrency", str(concurrency), "--load-timeout-ms", str(load_timeout_ms)]
            if os.name != "nt" and hasattr(os, "geteuid") and os.geteuid() == 0:
                args.append("--no-sandbox")
            kwargs = {"env": env, "cwd": ROOT}
            if os.name == "nt":
                kwargs["creationflags"] = subprocess.CREATE_NO_WINDOW | subprocess.CREATE_NEW_PROCESS_GROUP
            else:
                kwargs["start_new_session"] = True
            for reservation in reservations:
                reservation.close()
            with log_path.open("wb") as log:
                proc = subprocess.Popen(args, stdout=log, stderr=log, **kwargs)
                try:
                    deadline = time.monotonic() + 30
                    while time.monotonic() < deadline:
                        check(proc.poll() is None, f"Server exited during startup: {proc.returncode}")
                        try:
                            if session.get(f"http://{HOST}:{ports[0]}/health", timeout=1).status_code == 200:
                                break
                        except requests.RequestException:
                            pass
                        time.sleep(0.15)
                    else:
                        raise AssertionError("Server did not become ready within 30 seconds")
                    yield Api(session, ports, fixture.server_port)
                except BaseException:
                    log.flush()
                    print("\nServer log tail:\n" + log_path.read_text(encoding="utf-8", errors="replace")[-6000:])
                    raise
                finally:
                    stop_process(proc)
                    proc = None
    finally:
        stop_process(proc)
        session.close()
        for reservation in reservations:
            reservation.close()
        fixture.shutdown()
        fixture.server_close()
        fixture_thread.join(timeout=2)


class Api:
    def __init__(self, session, ports, fixture_port):
        self.session = session
        self.base = f"http://{HOST}:{ports[0]}"
        self.ws_url = f"ws://{HOST}:{ports[1]}"
        self.mcp_url = f"http://{HOST}:{ports[2]}/mcp"
        self.fixture = f"http://{HOST}:{fixture_port}"

    def render(self, path="/basic", **options):
        body = {"url": self.fixture + path, "output": "markdown", "settle_ms": 0,
                "long_poll_ms": 45000, **options}
        response = self.session.post(self.base + "/render", json=body, timeout=55)
        check(response.status_code == 200, f"HTTP render rejected: {response.status_code} {response.text[:500]}")
        result = response.json()
        check(result.get("state") == "succeeded", f"Render did not succeed: {result}")
        return result

    def result(self, task_id, output="markdown"):
        response = self.session.get(f"{self.base}/result/{task_id}", params={"output": output, "timeout": 45000}, timeout=55)
        response.raise_for_status()
        return response.json()

    def download(self, path):
        response = self.session.get(self.base + path, timeout=30)
        response.raise_for_status()
        return response.content


def metadata(result, converter="turndown", extractor="conservative"):
    meta = result.get("markdown_meta")
    check(isinstance(meta, dict), f"Missing markdown_meta: {result}")
    check(meta.get("converter") == converter, f"Wrong converter: {meta}")
    check(meta.get("extractor") == extractor, f"Wrong extractor: {meta}")
    check(isinstance(meta.get("warnings"), list), f"Missing warnings array: {meta}")
    check(isinstance(meta.get("conversion_ms"), (int, float)) and meta["conversion_ms"] >= 0,
          f"Invalid conversion timing: {meta}")
    return meta


class McpClient:
    def __init__(self, api):
        self.api = api
        self.headers = {"Content-Type": "application/json"}
        self.sequence = 0
        result = self.rpc("initialize", {"protocolVersion": "2025-03-26", "capabilities": {},
                          "clientInfo": {"name": "markdown-api-test", "version": "1"}})
        check("result" in result, f"MCP initialize failed: {result}")
        response = api.session.post(api.mcp_url, headers=self.headers,
                                    json={"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}}, timeout=10)
        response.raise_for_status()

    def rpc(self, method, params):
        self.sequence += 1
        response = self.api.session.post(self.api.mcp_url, headers=self.headers,
                    json={"jsonrpc": "2.0", "id": self.sequence, "method": method, "params": params}, timeout=55)
        response.raise_for_status()
        if response.headers.get("Mcp-Session-Id"):
            self.headers["Mcp-Session-Id"] = response.headers["Mcp-Session-Id"]
        return response.json()

    def tool(self, name, arguments):
        return self.rpc("tools/call", {"name": name, "arguments": arguments})


def mcp_text(response):
    check("error" not in response, f"MCP request failed: {response}")
    result = response.get("result", {})
    check(not result.get("isError"), f"MCP tool failed: {result}")
    return "\n".join(item.get("text", "") for item in result.get("content", []) if item.get("type") == "text")


def mcp_metadata(text):
    match = re.search(r"^\[markdown_meta: (.*)\]$", text, re.MULTILINE)
    check(match is not None, f"MCP metadata missing: {text[:500]}")
    return {"markdown_meta": json.loads(match.group(1))}


def test_http(api):
    result = api.render()
    metadata(result)
    markdown = result["markdown"]
    check("# Fixture heading" in markdown and "HIDDEN SENTINEL" not in markdown, "Heading/visibility conversion regressed")
    check("Visible aria sentinel" in markdown, "Visible aria-hidden content was removed")
    check(f"[Exact link]({api.fixture}/real)" in markdown, "Relative link or exact href extraction regressed")
    check("[x] Finished task" in markdown and "```js" in markdown and "| Alpha | 42 |" in markdown,
          "GFM tasks/code/table conversion regressed")
    check(api.result(result["task_id"])["markdown_meta"] == result["markdown_meta"], "HTTP result metadata changed")
    legacy = api.render(md_converter="legacy_cpp")
    metadata(legacy, "legacy_cpp")
    check("Fixture heading" in legacy["markdown"] and "/real)" in legacy["markdown"], "Legacy result missing content")
    serp = api.render(extract="baidu_serp")
    metadata(serp)
    check("Fixture heading" in serp.get("markdown", ""), "SERP extraction displaced requested Markdown output")
    article = api.render("/article", md_algorithm="readability")
    metadata(article, extractor="readability")
    check("Paragraph 5." in article["markdown"] and "NAVIGATION SENTINEL" not in article["markdown"], "Readability selection failed")
    conservative = api.render("/article")
    metadata(conservative)
    check("NAVIGATION SENTINEL" in conservative["markdown"], "Conservative removed visible page content")
    fallback = api.render("/empty", md_algorithm="readability")
    meta = metadata(fallback)
    check(meta.get("fallback_reason") and "readability_fallback_to_conservative" in meta["warnings"],
          "Readability fallback was not observable")
    empty = api.render("/empty")
    metadata(empty)
    check(not empty.get("markdown", ""), "Empty page gained fabricated Markdown")
    html = api.render(output="html")
    check("markdown_meta" not in html and "markdown" not in html, "HTML-only result leaked Markdown metadata")
    check("markdown_meta" not in api.result(html["task_id"]), "Unrequested Markdown metadata leaked on re-fetch")
    for invalid in ("unknown", "", None, True, 42, []):
        response = api.session.post(api.base + "/render", json={"url": api.fixture + "/basic", "md_converter": invalid}, timeout=10)
        check(response.status_code == 400 and "md_converter" in response.text, f"Invalid HTTP selector accepted: {invalid}")


def test_unicode(api):
    check(len(UNICODE_TEXT.encode("utf-8")) > 65536, "Unicode fixture too short")
    result = api.render("/unicode")
    metadata(result)
    check(result.get("markdown") == UNICODE_TEXT, "Large Unicode payload changed across chunk boundaries")
    legacy = api.render("/unicode", md_converter="legacy_cpp")
    metadata(legacy, "legacy_cpp")
    # The compatibility converter intentionally word-wraps paragraphs; only
    # whitespace differs, so verify every Unicode token survives in order.
    check(" ".join(legacy.get("markdown", "").split()) == UNICODE_TEXT, "Legacy Unicode payload changed")


def test_combined(api):
    baseline = api.render(output="html,screenshot", format="png")
    combined = api.render(output="html,markdown,pdf,screenshot", format="png")
    metadata(combined)
    check(combined["html"] == baseline["html"], "Markdown extraction mutated the live HTML")
    check("HIDDEN SENTINEL" in combined["html"] and "data-href" in combined["html"] and "<script>" in combined["html"],
          "HTML output lost original nodes/attributes")
    check(combined.get("has_pdf") and combined.get("has_image"), "Combined request lost an asynchronous output")
    check(api.download(combined["pdf"]).startswith(b"%PDF-"), "Invalid PDF output")
    image = api.download(combined["image"])
    check(image.startswith(b"\x89PNG\r\n\x1a\n"), "Invalid PNG output")
    check(image == api.download(baseline["image"]), "Markdown extraction changed screenshot pixels/encoding")


def test_ws(api):
    with closing(websocket.create_connection(api.ws_url, timeout=55, http_no_proxy=[HOST, "localhost"])) as ws:
        for invalid in ("wrong", None, 17):
            ws.send(json.dumps({"action": "render", "url": api.fixture + "/basic", "md_converter": invalid}))
            event = json.loads(ws.recv())
            check(event.get("event") == "error" and "md_converter" in event.get("message", ""), f"Invalid WS selector accepted: {event}")
        for converter in ("turndown", "legacy_cpp"):
            ws.send(json.dumps({"action": "render", "url": api.fixture + "/basic", "output": "markdown",
                                "md_converter": converter, "settle_ms": 0}))
            created = json.loads(ws.recv())
            check(created.get("event") == "created", f"WS task not created: {created}")
            finished = json.loads(ws.recv())
            check(finished.get("event") == "finished" and finished.get("state") == "succeeded", f"WS task failed: {finished}")
            check(finished.get("task_id") == created["task_id"], "WS completion attached to another task")
            metadata(finished, converter)
            fetched = api.result(created["task_id"])
            check(fetched.get("markdown_meta") == finished["markdown_meta"] and fetched.get("markdown"), "WS/HTTP results disagree")
        ws.send(json.dumps({"action": "render", "url": api.fixture + "/basic", "output": "html", "settle_ms": 0}))
        check(json.loads(ws.recv()).get("event") == "created", "WS HTML task not created")
        finished = json.loads(ws.recv())
        check(finished.get("state") == "succeeded" and "markdown_meta" not in finished, "HTML-only WS task leaked Markdown metadata")


def test_mcp(api):
    client = McpClient(api)
    listing = client.rpc("tools/list", {})
    tools = {tool["name"]: tool for tool in listing.get("result", {}).get("tools", [])}
    for name in ("render_url", "get_web_content"):
        selector = tools[name]["inputSchema"]["properties"]["md_converter"]
        check(selector.get("enum") == ["turndown", "legacy_cpp"] and selector.get("default") == "turndown",
              f"Incorrect MCP selector schema: {selector}")
        for invalid in ("wrong", None, 17):
            rejected = client.tool(name, {"url": api.fixture + "/basic", "md_converter": invalid})
            check(("error" in rejected or rejected.get("result", {}).get("isError")) and "md_converter" in json.dumps(rejected),
                  f"Invalid MCP selector accepted by {name}: {rejected}")
    for converter in ("turndown", "legacy_cpp"):
        text = mcp_text(client.tool("render_url", {"url": api.fixture + "/basic", "output": "markdown",
                                                    "md_converter": converter, "settle_ms": 0}))
        metadata(mcp_metadata(text), converter)
        check("Fixture heading" in text, "MCP render_url lost Markdown content")
        task_match = re.search(r"task_id=([a-zA-Z0-9]+)", text)
        check(task_match is not None, "MCP render_url omitted task ID")
        fetched = json.loads(mcp_text(client.tool("get_render_result", {"task_id": task_match.group(1), "output": "markdown"})))
        metadata(fetched, converter)
        reader_text = mcp_text(client.tool("get_web_content", {"url": api.fixture + "/article",
                                                              "md_converter": converter, "settle_ms": 0}))
        metadata(mcp_metadata(reader_text), converter, "readability")
        check("Paragraph 5." in reader_text and "NAVIGATION SENTINEL" not in reader_text, "MCP reader selection regressed")
    default_text = mcp_text(client.tool("render_url", {"url": api.fixture + "/basic", "output": "markdown", "settle_ms": 0}))
    metadata(mcp_metadata(default_text))
    html_text = mcp_text(client.tool("render_url", {"url": api.fixture + "/basic", "output": "html", "settle_ms": 0}))
    check("[markdown_meta:" not in html_text and "<h1>Fixture heading</h1>" in html_text, "MCP HTML-only output contract regressed")


def test_stale_callback(api):
    # Start Chromium before measuring the callback race; cold engine startup
    # can itself consume much of the deliberately short per-task timeout.
    response = api.session.post(api.base + "/render", json={"url": api.fixture + "/empty",
                                "output": "html", "settle_ms": 0, "long_poll_ms": 15000}, timeout=20)
    response.raise_for_status()
    warmup = response.json()
    check(warmup.get("state") == "succeeded" or
          (warmup.get("state") == "failed" and "timed out" in warmup.get("error", "")),
          f"Chromium warmup failed unexpectedly: {warmup}")
    tasks = []
    for path, settle_ms in (("/basic", 2500), ("/delayed", 0)):
        response = api.session.post(api.base + "/render", json={"url": api.fixture + path,
                                    "output": "markdown", "settle_ms": settle_ms}, timeout=10)
        response.raise_for_status()
        tasks.append(response.json()["task_id"])
    first = api.result(tasks[0])
    check(first.get("state") == "failed" and "timed out" in first.get("error", "") and
          "phase: collect" in first.get("error", ""),
          f"The first task did not time out during lazy-image preparation: {first}")
    second = api.result(tasks[1])
    check(second.get("state") == "succeeded", f"The next task failed after predecessor timeout: {second}")
    metadata(second)
    check(second.get("markdown") == "AFTER" and second.get("elapsed_ms", 0) >= 2400,
          f"The previous task's delayed callback collected the new page early: {second}")


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="backslashreplace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build" / ("seimi-render.exe" if os.name == "nt" else "seimi-render"))
    parser.add_argument("--qt-bin", type=Path, default=Path("C:/Qt/6.7.2/msvc2019_64/bin") if os.name == "nt" else None)
    args = parser.parse_args()
    check(args.binary.is_file(), f"Build executable first: {args.binary}")
    started = time.monotonic()
    with running_server(args.binary.resolve(), args.qt_bin) as api:
        for name, test in (("HTTP selectors, extraction and metadata", test_http),
                           ("large Unicode chunk fidelity", test_unicode),
                           ("combined HTML/Markdown/PDF/screenshot", test_combined),
                           ("WebSocket selectors and completion", test_ws),
                           ("MCP schemas, selectors and results", test_mcp)):
            case_started = time.monotonic()
            test(api)
            print(f"PASS {name} ({time.monotonic() - case_started:.2f}s)", flush=True)
    with running_server(args.binary.resolve(), args.qt_bin, concurrency=1, load_timeout_ms=4500) as api:
        case_started = time.monotonic()
        test_stale_callback(api)
        print(f"PASS stale callback after previous task timeout ({time.monotonic() - case_started:.2f}s)", flush=True)
    print(f"PASS all Markdown API checks ({time.monotonic() - started:.2f}s)")


if __name__ == "__main__":
    main()
