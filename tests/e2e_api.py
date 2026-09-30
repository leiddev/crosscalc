#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
crosscalc 端到端测试（HTTP 层）

做法：
  1. 以 --headless 启动【真实的】crosscalc 可执行文件（不起窗口，因此 CI 也能跑）；
  2. 等 /api/health 就绪；
  3. 把 tests/vectors/*.json 里的黄金向量逐条 POST 到 /api/eval，
     比对 ok / display / error_contains / value 容差；
  4. 顺带校验 /api/info、/api/platform、/api/functions、/api/assets 与静态资源。

与 crosscalc_tests --vectors 的区别：
  那边验证的是 C++ 引擎本身；这边验证的是"引擎 → HTTP → JSON"这条完整链路，
  包括参数解析（mode/base/wordsize 的字符串形式）、JSON 序列化、中文错误消息
  的转义等。两边用同一批向量，任何一边行为漂移都会被发现。

用法：
  python3 tests/e2e_api.py --binary build/crosscalc
  python3 tests/e2e_api.py --binary build/Release/crosscalc.exe --verbose
"""

import argparse
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
VECTOR_DIR = REPO_ROOT / "tests" / "vectors"


# --------------------------------------------------------------------- 工具 --
class Failures:
    def __init__(self):
        self.count = 0
        self.checks = 0

    def check(self, cond, message):
        self.checks += 1
        if not cond:
            self.count += 1
            print("  [失败] " + message)
        return cond


def free_port():
    """让操作系统给一个空闲端口，避免与本机已有服务撞车。"""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def http_json(url, payload=None, timeout=15):
    """GET 或 POST(JSON)，返回 (status, 解析后的 JSON 或 None)。"""
    data = None
    headers = {}
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read().decode("utf-8", errors="replace")
            return resp.status, json.loads(body)
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", errors="replace")
        try:
            return e.code, json.loads(body)
        except json.JSONDecodeError:
            return e.code, None


def http_raw(url, timeout=15):
    with urllib.request.urlopen(url, timeout=timeout) as resp:
        body = resp.read().decode("utf-8", errors="replace")
        return resp.status, body, resp.headers.get("Content-Type", "")


def wait_for_ready(port, proc, timeout=30.0):
    """轮询 /api/health；同时盯着子进程有没有提前退出。"""
    url = f"http://127.0.0.1:{port}/api/health"
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            return False, f"进程提前退出，返回码 {proc.returncode}"
        try:
            status, body = http_json(url, timeout=2)
            if status == 200 and body and body.get("status") == "ok":
                return True, ""
        except Exception:
            pass
        time.sleep(0.25)
    return False, f"等待 {timeout} 秒后服务仍未就绪"


# ----------------------------------------------------------------- 向量比对 --
def compare_case(fail, case, data, label, verbose):
    """把一次 /api/eval 的响应与向量期望比对。"""
    expect_error = bool(case.get("expect_error", False))
    ok = data.get("ok")

    if expect_error:
        fail.check(ok is False, f"{label}: 应当失败，实际 ok={ok}")
        if ok is False:
            needle = case.get("error_contains", "")
            err = data.get("error") or ""
            if needle:
                fail.check(needle in err, f"{label}: 错误消息应包含 {needle!r}，实际 {err!r}")
            fail.check(bool(err), f"{label}: 错误消息不应为空")
            # 中文提示检查（只要出现非 ASCII 就说明经过了翻译层）
            if case.get("require_chinese", False):
                fail.check(any(ord(c) > 127 for c in err),
                           f"{label}: 错误消息应为中文，实际 {err!r}")
        return

    if not fail.check(ok is True,
                      f"{label}: 应当成功，实际 ok={ok} error={data.get('error')!r}"):
        return

    if "expect" in case:
        got = data.get("display")
        want = case["expect"]
        if not fail.check(got == want, f"{label}: display 期望 {want!r}，实际 {got!r}"):
            return

    if "expect_value" in case:
        want = float(case["expect_value"])
        tol = float(case.get("tolerance", 1e-9))
        got = data.get("value")
        if isinstance(got, (int, float)):
            fail.check(abs(float(got) - want) <= tol,
                       f"{label}: value 期望 {want}，实际 {got}（容差 {tol}）")
        else:
            fail.check(False, f"{label}: value 不是数值：{got!r}")

    if verbose:
        print(f"    ok  {label} = {data.get('display')}")


def run_vectors(fail, port, files, supports_64bit, verbose):
    total = 0
    skipped = 0
    for path in files:
        doc = json.loads(Path(path).read_text(encoding="utf-8"))
        defaults = doc.get("defaults", {})
        name = doc.get("name", Path(path).name)
        print(f"\n=== 向量文件: {name} ({path}) ===")

        for i, case in enumerate(doc.get("cases", []), start=1):
            expr = case.get("expr", "")
            label = f"#{i} {expr!r}"

            if case.get("requires_64bit") and not supports_64bit:
                skipped += 1
                if verbose:
                    print(f"    跳过 {label}（本平台不支持 64 位精确整数）")
                continue

            # arith 门控：用例只在指定运算精度下成立。
            # "double"   = 引擎内部是 double（Windows/MSVC）
            # "extended" = 引擎内部是 80 位扩展精度（x86-64 Linux/macOS）
            # 与 vector_runner.cpp 里的处理必须保持一致，否则同一条用例
            # 在 C++ 侧跳过、在 Python 侧却当失败计，两边结论就对不上了。
            arith = case.get("arith", "")
            if arith:
                if arith not in ("double", "extended"):
                    fail(f"{label}: arith 只能是 \"double\" 或 \"extended\"")
                    continue
                wide = supports_64bit
                if (arith == "extended") != wide:
                    skipped += 1
                    if verbose:
                        print(f"    跳过 {label}（只适用于 {arith} 运算精度，"
                              f"本平台是 {'extended' if wide else 'double'}）")
                    continue

            payload = {
                "expression": expr,
                "mode": case.get("mode", defaults.get("mode", "standard")),
                "base": case.get("base", defaults.get("base", "dec")),
                "wordsize": str(case.get("wordsize", defaults.get("wordsize", "32"))),
            }

            total += 1
            try:
                status, data = http_json(f"http://127.0.0.1:{port}/api/eval", payload)
            except Exception as e:  # noqa: BLE001
                fail.check(False, f"{label}: 请求异常 {e}")
                continue

            if not fail.check(status == 200, f"{label}: HTTP 状态应为 200，实际 {status}"):
                continue
            if data is None:
                fail.check(False, f"{label}: 响应不是合法 JSON")
                continue

            compare_case(fail, case, data, label, verbose)

    print(f"\n向量用例 {total} 条（跳过 {skipped} 条）")
    return total, skipped


# ------------------------------------------------------------ API / 资源检查 --
def run_api_checks(fail, port, verbose):
    base = f"http://127.0.0.1:{port}"

    status, info = http_json(base + "/api/info")
    fail.check(status == 200, f"/api/info 状态应为 200，实际 {status}")
    if info:
        for key in ("name", "version", "os", "platform", "tinyexpr-plusplus"):
            fail.check(key in info, f"/api/info 缺少字段 {key}")
        fail.check(info.get("name") == "crosscalc", "/api/info name 应为 crosscalc")
        fail.check(isinstance(info.get("platform"), dict), "/api/info platform 应为对象")
        if verbose:
            print(f"    /api/info -> {json.dumps(info, ensure_ascii=False)[:160]}…")

    status, platform = http_json(base + "/api/platform")
    fail.check(status == 200, f"/api/platform 状态应为 200，实际 {status}")
    fail.check(isinstance(platform, dict) and "supports_64bit" in platform,
               "/api/platform 应包含 supports_64bit")

    status, funcs = http_json(base + "/api/functions")
    fail.check(status == 200, f"/api/functions 状态应为 200，实际 {status}")
    text = (funcs or {}).get("text", "")
    fail.check(len(text) > 200, "/api/functions 内容过短")
    for fn in ("sin", "bitand", "hex2dec", "fac", "mod", "if"):
        fail.check(fn in text, f"/api/functions 应包含 {fn}")

    status, assets = http_json(base + "/api/assets")
    fail.check(status == 200, f"/api/assets 状态应为 200，实际 {status}")
    paths = {f["path"] for f in (assets or {}).get("files", [])}
    for want in ("index.html", "app.js", "style.css"):
        fail.check(want in paths, f"/api/assets 应包含 {want}，实际 {sorted(paths)}")

    # 静态资源
    for path, want_cn in (("/", "text/html"), ("/app.js", "javascript"),
                          ("/style.css", "text/css")):
        status, body, ctype = http_raw(base + path)
        fail.check(status == 200, f"GET {path} 状态应为 200，实际 {status}")
        fail.check(want_cn in ctype, f"GET {path} Content-Type 应含 {want_cn}，实际 {ctype}")
        fail.check(len(body) > 100, f"GET {path} 内容过短")
    # 前端页面应当是 UTF-8 的中文页面
    status, body, _ = http_raw(base + "/")
    fail.check('charset="utf-8"' in body.lower(),
               "index.html 应声明 <meta charset='utf-8'>")
    fail.check("计算器" in body, "index.html 应包含中文界面文案")

    # 参数校验：非法 mode 必须被拒绝，而不是悄悄退回默认值
    status, data = http_json(base + "/api/eval", {"expression": "1+1", "mode": "bogus"})
    fail.check(status == 400, f"非法 mode 状态应为 400，实际 {status}")
    fail.check((data or {}).get("ok") is False, "非法 mode 应返回 ok=false")

    status, data = http_json(base + "/api/eval", {"expression": "1+1", "wordsize": "7"})
    fail.check(status == 400, f"非法 wordsize 状态应为 400，实际 {status}")

    # GET 形式
    status, data = http_json(base + "/api/eval?expr=2%2B3")
    fail.check(status == 200 and (data or {}).get("display") == "5",
               f"GET /api/eval?expr=2+3 应得 5，实际 {data}")

    # 程序员模式应带 all_bases
    status, data = http_json(base + "/api/eval",
                             {"expression": "255", "mode": "programmer",
                              "base": "hex", "wordsize": "32"})
    if fail.check(status == 200 and (data or {}).get("ok") is True,
                  f"程序员模式求值 255 应成功，实际 {data}"):
        fail.check(data.get("display") == "FF", f"255 的 HEX 应为 FF，实际 {data.get('display')}")
        ab = data.get("all_bases") or {}
        fail.check(ab.get("hex") == "FF", f"all_bases.hex 应为 FF，实际 {ab.get('hex')}")
        fail.check(ab.get("dec") == "255", f"all_bases.dec 应为 255，实际 {ab.get('dec')}")
        fail.check(ab.get("oct") == "377", f"all_bases.oct 应为 377，实际 {ab.get('oct')}")
        fail.check(ab.get("bin") == "1111 1111",
                   f"all_bases.bin 应为 '1111 1111'，实际 {ab.get('bin')}")
    else:
        print(f"    (调试) 实际响应: {json.dumps(data, ensure_ascii=False)}")

    # 空表达式
    status, data = http_json(base + "/api/eval", {"expression": "   "})
    fail.check(status == 200 and (data or {}).get("ok") is False,
               "空表达式应返回 200 + ok=false")
    fail.check("请输入" in ((data or {}).get("error") or ""),
               f"空表达式提示应为中文，实际 {(data or {}).get('error')!r}")

    # 请求体不是对象
    status, data = http_json(base + "/api/eval", [1, 2, 3])
    fail.check(status == 400, f"非对象请求体状态应为 400，实际 {status}")


# ---------------------------------------------------------------------- 主流程 --
def find_binary(explicit):
    if explicit:
        p = Path(explicit)
        if p.is_file():
            return p
        sys.exit(f"找不到可执行文件: {explicit}")

    candidates = [
        REPO_ROOT / "build" / "Release" / "crosscalc.exe",
        REPO_ROOT / "build" / "Release" / "crosscalc",
        REPO_ROOT / "build" / "crosscalc.exe",
        REPO_ROOT / "build" / "crosscalc",
        REPO_ROOT / "build" / "Debug" / "crosscalc.exe",
        REPO_ROOT / "build-linux" / "crosscalc",
    ]
    for c in candidates:
        if c.is_file():
            return c
    sys.exit("找不到 crosscalc 可执行文件，请用 --binary 指定（先构建：cmake --build build --config Release）")


def main():
    # Windows 控制台默认代码页不是 UTF-8（管道输出时尤其明显），
    # 这里强制让本脚本的输出走 UTF-8，中文才不会变成乱码。
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    ap = argparse.ArgumentParser(description="crosscalc 端到端测试（HTTP 层）")
    ap.add_argument("--binary", help="crosscalc 可执行文件路径")
    ap.add_argument("--vectors", nargs="*", help="向量文件（默认 tests/vectors/*.json）")
    ap.add_argument("--port", type=int, default=0, help="固定端口（默认自动挑空闲端口）")
    ap.add_argument("--verbose", action="store_true", help="打印每条通过用例")
    ap.add_argument("--timeout", type=float, default=30.0, help="等待服务就绪的秒数")
    args = ap.parse_args()

    binary = find_binary(args.binary)
    vectors = args.vectors or sorted(str(p) for p in VECTOR_DIR.glob("*.json"))
    if not vectors:
        sys.exit(f"没有找到向量文件: {VECTOR_DIR}")

    port = args.port or free_port()
    print(f"启动: {binary} --headless --port {port}")

    # 注意：不要把子进程 stdout 接到 subprocess.PIPE 上却不读它 ——
    # 程序会为每个 HTTP 请求写一行日志，管道缓冲区（Windows 64KB）写满后
    # 进程会阻塞在 write 上，后续请求就永远得不到响应（表现为脚本"卡死"）。
    # 因此这里落到临时文件，既不会阻塞，出错时也能把日志打出来。
    log_file = tempfile.NamedTemporaryFile(
        mode="w+", encoding="utf-8", suffix=".log", delete=False,
        prefix="crosscalc-e2e-")
    log_path = log_file.name

    proc = subprocess.Popen(
        [str(binary), "--headless", "--port", str(port)],
        stdout=log_file,
        stderr=subprocess.STDOUT,
        cwd=str(REPO_ROOT),
        text=True,
        encoding="utf-8",
        errors="replace",
    )

    def read_log():
        try:
            log_file.flush()
            return Path(log_path).read_text(encoding="utf-8", errors="replace")
        except Exception:
            return ""

    fail = Failures()
    ready = False
    try:
        ready, why = wait_for_ready(port, proc, args.timeout)
        if not ready:
            proc.kill()
            sys.exit(f"服务未就绪：{why}\n---- 进程输出 ----\n{read_log()}")

        # 平台能力
        _, platform = http_json(f"http://127.0.0.1:{port}/api/platform")
        supports_64 = bool((platform or {}).get("supports_64bit"))
        print(f"平台: supports_64bit={supports_64} "
              f"max_integer_bitness={(platform or {}).get('max_integer_bitness')}")

        print("\n=== API / 静态资源检查 ===")
        run_api_checks(fail, port, args.verbose)

        run_vectors(fail, port, vectors, supports_64, args.verbose)

    finally:
        try:
            proc.terminate()
            proc.wait(timeout=10)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass
        try:
            log_file.close()
            os.unlink(log_path)
        except Exception:
            pass

    print("\n" + "-" * 58)
    print(f"  端到端断言 {fail.checks} 条，失败 {fail.count} 条")
    print("-" * 58)
    if fail.count == 0:
        print("\nALL E2E TESTS PASSED")
        return 0
    print("\nE2E TESTS FAILED")
    return 1


if __name__ == "__main__":
    sys.exit(main())
