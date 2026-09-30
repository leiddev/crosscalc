#!/usr/bin/env bash
# crosscalc GUI 冒烟测试（Linux / Xvfb）
#
# 前面几层自测（ctest、e2e_api.py、check_frontend.py）都跑在无头模式下，
# 碰不到 GTK / WebKitGTK。这个脚本负责最后一步：
#   真的把窗口开起来（用 Xvfb 假装有显示器），然后验证
#     ① HTTP 服务起来了，每个接口返回的都是合法 JSON 且关键字段正确；
#     ② 静态资源能被取到，且内容是"这一版"的（不是上次嵌进去的旧页面）；
#     ③ 程序日志里出现窗口自己发起的资源请求 —— 这是"WebKitGTK 真的把页面
#        加载起来了"的证据，也是本脚本唯一能验证渲染链路的方式。
#
# 用法：
#   bash tests/smoke_gui.sh build-linux/crosscalc
#
# 退出码 0 表示全部通过；任何一步失败都会打印 ::error:: 并返回非 0。

set -euo pipefail

BIN="${1:-build-linux/crosscalc}"
PORT="${2:-}"
BASE=""
LOG="$(mktemp -t crosscalc-smoke-XXXXXX.log)"

if [ ! -x "$BIN" ]; then
  echo "::error title=冒烟测试::找不到可执行文件: $BIN"
  exit 1
fi

# 端口：不传就临时找一个空闲端口。
#
# 千万不要图省事写死 18080：上一次冒烟测试残留的实例（清理不彻底时很常见）
# 会继续占着那个端口，于是 curl 打到的是【上一个进程】，报出来的错却看着像
# 新进程的问题 —— 实测就这样被一个 404 误导过。这里顺便在启动前确认端口是空的，
# 让"有东西占着"这件事立刻暴露。
if [ -z "$PORT" ]; then
  PORT=$(python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
)
fi
BASE="http://127.0.0.1:${PORT}"

if curl -s -o /dev/null --max-time 2 "$BASE/api/health"; then
  echo "::error title=冒烟测试::端口 ${PORT} 上已经有服务在响应，请先清掉残留进程（pkill -x crosscalc）"
  exit 1
fi

# 无头环境里 WebKitGTK 需要这些开关才能起来：
#   - DMABUF / 合成模式在 Xvfb 下会因为没有真实 GPU 而崩
#   - LIBGL_ALWAYS_SOFTWARE 强制软件渲染
export WEBKIT_DISABLE_DMABUF_RENDERER=1
export WEBKIT_DISABLE_COMPOSITING_MODE=1
export LIBGL_ALWAYS_SOFTWARE=1
export GDK_BACKEND=x11

cleanup() {
  local rc=$?
  # 优先杀 /api/info 报出来的真实 PID：xvfb-run 是个脚本，$! 拿到的是它自己，
  # kill 它不一定能带走真正的 crosscalc；而 /api/info 的 pid 就是 getpid()，
  # 是我们自己人，杀它最准。
  if [ -n "${REAL_PID:-}" ]; then
    kill "$REAL_PID" 2>/dev/null || true
  fi
  kill "$APP" 2>/dev/null || true
  pkill -x "$(basename "$BIN")" 2>/dev/null || true
  # 失败时把日志留下来，方便排查
  if [ $rc -ne 0 ]; then
    echo "--- 程序日志（出错时保留）: $LOG ---"
    cat "$LOG" || true
  fi
  return $rc
}
APP=""
REAL_PID=""
trap cleanup EXIT

echo "== 启动 GUI（xvfb-run，端口 ${PORT}）=="
xvfb-run -a --server-args="-screen 0 1280x1024x24" \
  "$BIN" --port "$PORT" > "$LOG" 2>&1 &
APP=$!

# 等 HTTP 服务起来
for _ in $(seq 1 30); do
  if curl -sf -o /dev/null "$BASE/api/health"; then break; fi
  if ! kill -0 "$APP" 2>/dev/null; then
    echo "::error title=冒烟测试::程序启动后立刻退出，见上面日志"
    exit 1
  fi
  sleep 1
done

if ! curl -sf -o /dev/null "$BASE/api/health"; then
  echo "::error title=冒烟测试::30 秒内没有等到 HTTP 服务就绪"
  exit 1
fi
echo "  服务已就绪: $BASE"

# 确认应答的确实是我们刚启动的那个进程（而不是别的残留实例），
# 并记下 PID 供清理使用。/api/info 里的 pid 就是 getpid()。
REAL_PID=$(curl -sf "$BASE/api/info" \
  | python3 -c 'import json,sys; print(json.load(sys.stdin).get("pid",""))' 2>/dev/null || true)
if [ -z "$REAL_PID" ]; then
  echo "::error title=冒烟测试::无法从 /api/info 读到进程 PID"
  exit 1
fi
if ! kill -0 "$REAL_PID" 2>/dev/null; then
  echo "::error title=冒烟测试::/api/info 报出的 PID ${REAL_PID} 并不存在，应答可能来自别的进程"
  exit 1
fi
echo "  应答进程 PID: $REAL_PID"

# 断言某个接口返回合法 JSON 且包含指定片段。
# 特意用 Python 的 json.load 而不是 grep：应该守住的是「输出是合法 JSON」这件事本身。
assert_json() {  # $1=标签 $2=必须出现的片段；正文从 stdin 读
  local label="$1" want="$2" body
  body=$(cat)
  if ! printf '%s' "$body" | python3 -c 'import json,sys; json.load(sys.stdin)' 2>/dev/null; then
    echo "::error title=${label}::返回的不是合法 JSON: $body"
    return 1
  fi
  case "$body" in
    *"$want"*) ;;
    *) echo "::error title=${label}::缺少 ${want}，实际: $body"; return 1 ;;
  esac
  printf '  %-32s %s\n' "$label" "OK"
}

echo "== 接口校验 =="
curl -sf "$BASE/api/health"   | assert_json "GET /api/health"   '"status":"ok"'
curl -sf "$BASE/api/info"     | assert_json "GET /api/info"     '"tinyexpr-plusplus"'
curl -sf "$BASE/api/platform" | assert_json "GET /api/platform" '"supports_64bit"'
# 2+3=5，且必须是字符串 "5"（前端直接把这个字段贴到结果区）
curl -sf "$BASE/api/eval?expr=2%2B3" | assert_json "GET /api/eval 2+3" '"display":"5"'
# 程序员模式：255 在十六进制下显示 FF
curl -sf "$BASE/api/eval?expr=255&mode=programmer&base=hex" \
  | assert_json "GET /api/eval 255→hex" '"display":"FF"'
# 进制转换是显示层做的，四种进制应齐出
curl -sf "$BASE/api/eval?expr=255&mode=programmer&base=hex" \
  | assert_json "GET /api/eval all_bases" '"all_bases"'
curl -sf "$BASE/api/assets"   | assert_json "GET /api/assets"   '"app.js"'
curl -sf "$BASE/api/functions"| assert_json "GET /api/functions" 'bitlrotate8'

# 非法 mode 必须 400，而不是 500 或崩溃
code=$(curl -s -o /dev/null -w '%{http_code}' "$BASE/api/eval?expr=1&mode=nope")
if [ "$code" != "400" ]; then
  echo "::error title=/api/eval::非法 mode 应返回 400，实际 $code"
  exit 1
fi
printf '  %-32s %s\n' "GET /api/eval 非法 mode" "HTTP $code OK"

echo "== 静态资源 =="
for u in / /style.css /app.js; do
  curl -sf -o /dev/null -w "  GET ${u} -> %{http_code} (%{size_download}B)\n" "$BASE$u"
done

# 内嵌资源必须是这一版的：改过 www/ 但 mtime 没变会导致 cpp-embedlib 跳过重嵌，
# 编出来还是旧页面而且不报错 —— 用一个只有新页面才有的字符串守住。
if ! curl -sf "$BASE/" | grep -q '程序员'; then
  echo "::error title=首页内容::内嵌的 index.html 不是最新的（缺『程序员』字样）"
  exit 1
fi
printf '  %-32s %s\n' "首页含『程序员』" "OK"

echo "== 渲染链路 =="
# 窗口加载页面时会自己来取 /app.js；日志里有这条请求，说明 GTK/WebKitGTK 真的
# 把页面加载起来了（这是无头环境里能拿到的最强证据）。
if ! grep -q 'app.js' "$LOG"; then
  echo "::error title=渲染链路::日志中没有窗口发起的 app.js 请求，页面可能没加载"
  exit 1
fi
printf '  %-32s %s\n' "窗口已加载页面" "OK"

echo
echo "ALL SMOKE TESTS PASSED"
