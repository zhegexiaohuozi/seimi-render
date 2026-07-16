#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 wanghaomiao.cn
# SPDX-License-Identifier: Apache-2.0
#
# seimi-render 冒烟测试：健康检查、单任务渲染、长轮询、并发、WebSocket 推送
# 用法: scripts/smoke_test.sh [http_port] [ws_port]
# 前置: 服务已在对应端口运行。
set -uo pipefail

HTTP_PORT="${1:-8088}"
WS_PORT="${2:-8089}"
HOST="127.0.0.1"
BASE="http://$HOST:$HTTP_PORT"
PASS=0; FAIL=0

ok()   { echo "  ✓ $1"; PASS=$((PASS+1)); }
bad()  { echo "  ✗ $1"; FAIL=$((FAIL+1)); }

echo "== seimi-render 冒烟测试 ($BASE) =="

# 0. 健康检查
if curl -sf --max-time 3 "$BASE/" | grep -q '"status":"ok"'; then
    ok "健康检查 /"
else
    bad "健康检查失败（服务未运行？）"; exit 1
fi

# 1. 单任务 + 长轮询拉 HTML
echo "- 单任务渲染 example.com（长轮询）..."
RESP=$(curl -s --max-time 30 -X POST "$BASE/render" -H "Content-Type: application/json" \
    -d '{"url":"https://example.com","settle_ms":1500,"long_poll_ms":25000}')
STATE=$(echo "$RESP" | python3 -c "import json,sys;print(json.load(sys.stdin).get('state',''))" 2>/dev/null)
HLEN=$(echo "$RESP" | python3 -c "import json,sys;print(len(json.load(sys.stdin).get('html','')))" 2>/dev/null)
if [[ "$STATE" == "succeeded" && "$HLEN" -gt 0 ]]; then ok "单任务渲染成功 (html=${HLEN}B)"; else bad "单任务渲染失败 (state=$STATE)"; fi

# 2. 状态查询端点
TID=$(echo "$RESP" | python3 -c "import json,sys;print(json.load(sys.stdin)['task_id'])" 2>/dev/null)
if curl -sf --max-time 3 "$BASE/status/$TID" | grep -q "$TID"; then ok "状态查询 /status/:id"; else bad "状态查询失败"; fi

# 3. 并发
echo "- 并发渲染 4 个 example.com..."
CTIDS=""
for i in 1 2 3 4; do
    CTIDS="$CTIDS $(curl -s --max-time 5 -X POST "$BASE/render" -H "Content-Type: application/json" -d '{"url":"https://example.com"}' | python3 -c "import json,sys;print(json.load(sys.stdin)['task_id'])" 2>/dev/null)"
done
sleep 12
COK=0
for t in $CTIDS; do
    [[ -z "$t" ]] && continue
    S=$(curl -s --max-time 3 "$BASE/status/$t" | python3 -c "import json,sys;print(json.load(sys.stdin)['state'])" 2>/dev/null)
    [[ "$S" == "succeeded" ]] && COK=$((COK+1))
done
if [[ "$COK" -ge 3 ]]; then ok "并发渲染 $COK/4 成功"; else bad "并发渲染仅 $COK/4 成功"; fi

# 4. 队列统计
if curl -sf --max-time 3 "$BASE/stats" | grep -q "total"; then ok "统计 /stats"; else bad "统计失败"; fi

# 5. WebSocket 推送
echo "- WebSocket 推送..."
if command -v python3 >/dev/null; then
    WS_TEST=$(mktemp /tmp/seimi_ws_XXXX.py)
    cat > "$WS_TEST" <<'PYEOF'
import socket, base64, os, struct, sys, time, json
def main():
    port=int(sys.argv[1]); task_id=sys.argv[2]; wait=float(sys.argv[3])
    s=socket.create_connection(("127.0.0.1",port),timeout=5)
    key=base64.b64encode(os.urandom(16)).decode()
    s.sendall(f"GET / HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n".encode())
    d=b""
    while b"\r\n\r\n" not in d: d+=s.recv(4096)
    if b"101" not in d.split(b"\r\n")[0]: print("NO"); return
    def send(text):
        p=text.encode(); h=bytearray([0x81]); m=os.urandom(4); l=len(p)
        if l<126: h.append(0x80|l)
        elif l<65536: h.append(0x80|126); h+=struct.pack(">H",l)
        else: h.append(0x80|127); h+=struct.pack(">Q",l)
        h+=m; s.sendall(bytes(h)+bytes(b^m[i%4] for i,b in enumerate(p)))
    def recv():
        s.recv(1); h1=s.recv(1); ln=h1[0]&0x7f
        if ln==126: ln=struct.unpack(">H",s.recv(2))[0]
        elif ln==127: ln=struct.unpack(">Q",s.recv(8))[0]
        p=b""
        while len(p)<ln: p+=s.recv(ln-len(p))
        return p.decode(errors="replace")
    send(json.dumps({"action":"subscribe","task_id":task_id}))
    s.settimeout(wait); t0=time.time()
    while time.time()-t0<wait:
        try:
            m=recv()
            if '"finished"' in m: print("OK"); return
        except socket.timeout: pass
    print("NO")
main()
PYEOF
    WTID=$(curl -s --max-time 5 -X POST "$BASE/render" -H "Content-Type: application/json" -d '{"url":"https://example.com"}' | python3 -c "import json,sys;print(json.load(sys.stdin)['task_id'])" 2>/dev/null)
    WRES=$(python3 "$WS_TEST" "$WS_PORT" "$WTID" 15 2>/dev/null)
    rm -f "$WS_TEST"
    [[ "$WRES" == "OK" ]] && ok "WebSocket 推送收到 finished 事件" || bad "WebSocket 推送未收到"
else
    echo "  (跳过 WebSocket 测试：无 python3)"
fi

echo ""
echo "== 结果: 通过=$PASS 失败=$FAIL =="
[[ "$FAIL" -eq 0 ]] && exit 0 || exit 1
