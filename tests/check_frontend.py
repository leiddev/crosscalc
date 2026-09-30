#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
crosscalc 前端与引擎一致性静态检查

这个脚本回答一个很实际的问题：**前端键盘上的按钮，引擎真的认识吗？**

前端里那些"插进表达式"的文本（如 sin( ）本质上是对引擎 API 的硬编码调用。
一旦拼错一个函数名，或者引擎升级后去掉了某个函数，界面上不会报错 ——
用户点了按钮，只会看到一个莫名其妙的错误提示。这类问题单测抓不到
（单测直接调引擎，不经过前端），所以用静态检查补上。

检查项：
  1. app.js 里所有 $('#id') / getElementById('id') 引用的元素，
     必须真的存在于 index.html；
  2. 键盘按钮插入的函数名，必须出现在引擎的函数清单里；
  3. 键盘按钮插入的常量名（pi / e）必须在引擎的常量清单里；
  4. 模式名（standard/scientific/programmer）在 HTML、CSS、app.js 三处一致；
  5. 进制名（hex/dec/oct/bin）在 HTML 与 app.js 中一致；
  6. 字长取值（8/16/32/64）在 HTML 与 app.js 中一致。

引擎清单的来源：直接运行 crosscalc_tests --list-functions（不依赖网络与 GUI），
也可以用 --functions-file 指定一个预先保存的清单文本。

用法：
  python3 tests/check_frontend.py --tests-binary build/Release/crosscalc_tests.exe
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
WWW = REPO_ROOT / "www"


class Failures:
    def __init__(self):
        self.checks = 0
        self.count = 0

    def check(self, cond, message):
        self.checks += 1
        if not cond:
            self.count += 1
            print("  [失败] " + message)
        return bool(cond)

    def info(self, message, verbose):
        if verbose:
            print("    " + message)


# ------------------------------------------------------------- 引擎清单解析 --
def load_engine_symbols(functions_file, tests_binary, verbose):
    """解析 crosscalc_tests --list-functions 的输出，返回 (函数集合, 常量集合)。"""
    if functions_file:
        text = Path(functions_file).read_text(encoding="utf-8")
    else:
        if not tests_binary or not Path(tests_binary).is_file():
            sys.exit("找不到 crosscalc_tests，请用 --tests-binary 指定，"
                     "或用 --functions-file 提供一个已保存的清单")
        proc = subprocess.run([str(tests_binary), "--list-functions"],
                              capture_output=True, text=True, encoding="utf-8",
                              errors="replace", timeout=60)
        if proc.returncode != 0:
            sys.exit(f"--list-functions 执行失败：{proc.stderr}")
        text = proc.stdout

    funcs, consts = set(), set()
    current = None
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        if line.endswith(":"):
            low = line.lower()
            if "function" in low:
                current = funcs
            elif "constant" in low or "variable" in low:
                current = consts
            else:
                current = None
            continue
        if current is not None:
            current.add(line)

    if verbose:
        print(f"    引擎提供 {len(funcs)} 个函数（常量 pi/e 也在其中）、"
              f"{len(consts)} 个自定义变量（{sorted(consts)}）")
    return funcs, consts


# --------------------------------------------------------------- 前端解析 --
def read_frontend(verbose):
    html_file = WWW / "index.html"
    js_file = WWW / "app.js"
    css_file = WWW / "style.css"
    for p in (html_file, js_file, css_file):
        if not p.is_file():
            sys.exit(f"缺少前端文件: {p}")
    html = html_file.read_text(encoding="utf-8")
    js = js_file.read_text(encoding="utf-8")
    css = css_file.read_text(encoding="utf-8")
    if verbose:
        print(f"    index.html {len(html)} 字节, app.js {len(js)} 字节, "
              f"style.css {len(css)} 字节")
    return html, js, css


def html_ids(html):
    return set(re.findall(r'\bid="([^"]+)"', html))


def js_referenced_ids(js):
    """app.js 用到的所有元素 id。"""
    ids = set()
    ids |= set(re.findall(r"""\$\('#([A-Za-z0-9_-]+)'\)""", js))
    ids |= set(re.findall(r"""\$\("#([A-Za-z0-9_-]+)"\)""", js))
    ids |= set(re.findall(r"""getElementById\(\s*['"]([A-Za-z0-9_-]+)['"]""", js))
    ids |= set(re.findall(r"""querySelector\(\s*['"]#([A-Za-z0-9_-]+)['"]""", js))
    # 模板串里拼出来的（例如 `.base-row[data-base=...]`）不算 id，跳过
    return ids


def keypad_inserts(js):
    """
    从 KEYPADS 定义里抽出所有按键的插入文本，返回 (静态插入, 动态生成的名字)。

    动态的是 bitFunc('bitnot') 这类：实际插入的是 bitnot8 / bitnot16 / ...，
    所以要把名字展开成四个字长的变体一起校验。
    """
    start = js.find("const KEYPADS")
    end = js.find("function k(label")
    if start < 0 or end < 0 or end < start:
        sys.exit("无法在 app.js 中定位 KEYPADS 定义（脚本需要同步更新）")
    block = js[start:end]

    static = re.findall(r"""k\(\s*'[^']*'\s*,\s*'([^']*)'""", block)
    dynamic = re.findall(r"""k\(\s*'[^']*'\s*,\s*bitFunc\('([^']+)'\)""", block)
    # 形如 (s) => `...` 的其它动态插入一并收集（当前没有，留作扩展）
    return static, dynamic


def expand_dynamic(name):
    """bitFunc('bitnot') -> {bitnot8, bitnot16, bitnot32, bitnot64}"""
    return {f"{name}{bits}" for bits in (8, 16, 32, 64)}


# ------------------------------------------------------------------ 各检查项 --
def check_ids(fail, html, js, verbose):
    have = html_ids(html)
    want = js_referenced_ids(js)
    missing = sorted(want - have)
    fail.check(not missing,
               f"app.js 引用了 index.html 中不存在的元素 id: {missing}")
    fail.info(f"元素 id 引用 {len(want)} 个，全部存在", verbose)


# onKey() 里被当成"动作"处理、而不是插入文本的特殊 insert 值
CONTROL_INSERTS = {"clear", "backspace", "negate", "equals"}

def check_keypad_functions(fail, js, funcs, consts, verbose):
    static, dynamic = keypad_inserts(js)

    # 以 '(' 结尾的插入 = 调用某个函数。
    # 先排除动作键、数字键和 '' / '(' 这种纯文本插入。
    calls = []
    for s in static:
        if s in CONTROL_INSERTS or s in ("", "(", ")"):
            continue
        if re.fullmatch(r"digit[A-F]", s):   # 十六进制数字键
            continue
        if re.fullmatch(r"\d", s):           # 0-9 数字键
            continue
        if re.fullmatch(r"[a-zA-Z_][a-zA-Z0-9_]*\(", s):
            calls.append(s)
    called = {s[:-1] for s in calls}
    unknown = sorted(n for n in called if n not in funcs)
    fail.check(not unknown,
               f"键盘按钮插入了引擎不认识的函数: {unknown}")

    # 键盘用到的常量必须真的存在（tinyexpr 把它们和内置函数列在一起）
    for c in ("pi", "e"):
        if any(s == c for s in static):
            fail.check(c in funcs or c in consts, f"引擎不提供常量 {c!r}")

    for name in dynamic:
        for full in sorted(expand_dynamic(name)):
            fail.check(full in funcs,
                       f"键盘的 {name} 按钮在 {full} 字长下会插入引擎不认识的函数 {full}")

    # 不带括号的插入：可能是一个常量（pi / e），也可能只是运算符、动作键或数字键
    bare = [s for s in static if not s.endswith("(")]
    for s in bare:
        if s in CONTROL_INSERTS or re.fullmatch(r"digit[A-F]", s) or re.fullmatch(r"\d", s):
            continue
        if re.fullmatch(r"[a-zA-Z_][a-zA-Z0-9_]*", s):
            fail.check(s in consts or s in funcs,
                       f"键盘按钮插入了未知的符号: {s!r}（既不是函数也不是常量）")

    fail.info(f"校验按键函数调用 {len(called)} 个 + 动态 "
              f"{sum(len(expand_dynamic(n)) for n in dynamic)} 个", verbose)
    fail.info(f"按键里的其它插入: {bare}", verbose)


def check_modes(fail, html, css, js, verbose):
    modes = {"standard", "scientific", "programmer"}
    html_modes = set(re.findall(r'data-mode="([a-z]+)"', html))
    tab_modes = set(re.findall(r'class="mode-tab" data-mode="([a-z]+)"', html))
    css_modes = set(re.findall(r'body\[data-mode="([a-z]+)"\]', css))
    js_modes = set(re.findall(r"""\b(standard|scientific|programmer)\s*:\s*\[""", js))
    # 只有"科学/程序员"两种模式有专属面板，用 [data-only="..."] 标记
    only_modes = set(re.findall(r'data-only="([a-z]+)"', html))

    fail.check(tab_modes == modes,
               f"index.html 的模式页签应恰好是 {sorted(modes)}，实际 {sorted(tab_modes)}")
    fail.check(html_modes == modes,
               f"index.html 的 data-mode 取值应恰好是 {sorted(modes)}，"
               f"实际 {sorted(html_modes)}")
    fail.check(css_modes <= modes,
               f"style.css 出现了未知的模式选择器 {sorted(css_modes - modes)}")
    fail.check(js_modes == modes,
               f"app.js 的 KEYPADS 应覆盖 {sorted(modes)}，实际 {sorted(js_modes)}")
    # 每个 [data-only] 面板都必须有对应的样式规则在控制它的显隐
    fail.check(only_modes == css_modes,
               f"[data-only] 面板 {sorted(only_modes)} 与 style.css 的 "
               f"body[data-mode] 规则 {sorted(css_modes)} 不一致")
    fail.info(f"模式: html={sorted(html_modes)} css={sorted(css_modes)} "
              f"js={sorted(js_modes)} data-only={sorted(only_modes)}", verbose)


def check_bases(fail, html, js, verbose):
    base_values = {"hex", "dec", "oct", "bin"}
    html_bases = set(re.findall(r'data-base="([a-z]+)"', html))
    fail.check(html_bases == base_values,
               f"index.html 的 data-base 应恰好是 {sorted(base_values)}，"
               f"实际 {sorted(html_bases)}")

    # app.js 里的进制 -> 基数值映射
    radix_map = re.search(r"const radix = \{([^}]*)\}", js)
    fail.check(radix_map is not None, "app.js 中找不到进制映射 { bin/oct/dec/hex: 基数 }")
    if radix_map:
        keys = set(re.findall(r"(\w+)\s*:", radix_map.group(1)))
        fail.check(keys == base_values,
                   f"app.js 的进制映射键应为 {sorted(base_values)}，实际 {sorted(keys)}")
    fail.info(f"进制: html={sorted(html_bases)}", verbose)


def check_wordsizes(fail, html, js, verbose):
    want = {"8", "16", "32", "64"}
    html_ws = set(re.findall(r'data-wordsize="(\d+)"', html))
    fail.check(html_ws == want,
               f"index.html 的 data-wordsize 应恰好是 {sorted(want)}，"
               f"实际 {sorted(html_ws)}")
    # app.js 需要处理 64 位门控
    fail.check("supports_64bit" in js,
               "app.js 应当根据 supports_64bit 处理 QWORD 的可用性")
    fail.info(f"字长: html={sorted(html_ws)}", verbose)


def check_contracts(fail, js, verbose):
    """前端与后端约定的一些字符串必须对得上。"""
    for needle, why in (
        ("/api/eval", "求值接口路径"),
        ("/api/platform", "平台能力接口路径"),
        ("/api/functions", "函数清单接口路径"),
        ("/api/info", "信息接口路径"),
        ("cppEvaluate", "webview 绑定名（需与 src/main.cpp 中 w.bind 的一致）"),
    ):
        fail.check(needle in js, f"app.js 中缺少 {why}：{needle}")

    # 请求字段名必须与 C++ 端 build_request 解析的一致
    for field in ("expression", "mode", "base", "wordsize"):
        fail.check(f'"{field}"' in js or f"{field}:" in js,
                   f"app.js 未使用请求字段 {field}")
    fail.info("接口与字段契约检查完成", verbose)


# ---------------------------------------------------------------------- 主流程 --
def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    ap = argparse.ArgumentParser(description="crosscalc 前端与引擎一致性静态检查")
    ap.add_argument("--tests-binary", help="crosscalc_tests 可执行文件路径")
    ap.add_argument("--functions-file", help="预先保存的函数清单文本")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    tests_binary = args.tests_binary
    if not tests_binary and not args.functions_file:
        for cand in ("build/Release/crosscalc_tests.exe", "build/crosscalc_tests",
                     "build/Release/crosscalc_tests", "build/Debug/crosscalc_tests.exe"):
            p = REPO_ROOT / cand
            if p.is_file():
                tests_binary = str(p)
                break

    funcs, consts = load_engine_symbols(args.functions_file, tests_binary, args.verbose)
    html, js, css = read_frontend(args.verbose)

    fail = Failures()
    check_ids(fail, html, js, args.verbose)
    check_keypad_functions(fail, js, funcs, consts, args.verbose)
    check_modes(fail, html, css, js, args.verbose)
    check_bases(fail, html, js, args.verbose)
    check_wordsizes(fail, html, js, args.verbose)
    check_contracts(fail, js, args.verbose)

    print("\n" + "-" * 58)
    print(f"  前端一致性检查 {fail.checks} 条，失败 {fail.count} 条")
    print("-" * 58)
    if fail.count == 0:
        print("\nALL FRONTEND CHECKS PASSED")
        return 0
    print("\nFRONTEND CHECKS FAILED")
    return 1


if __name__ == "__main__":
    sys.exit(main())
