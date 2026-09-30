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
  6. 字长取值（8/16/32/64）在 HTML 与 app.js 中一致；
  7. app.js 里那些不依赖 DOM 的纯函数（进制重排等）用 Node 实跑一遍
     —— 能跑就做真实验证，没装 Node 则明确报告"跳过"。

前 6 项都是"照字符串比一比"，第 7 项才是真的执行代码：把二进制/八进制数字
换算成 0x 字面量这一步算错了，界面上完全看不出来（数值会静静变成另一个数），
所以必须拿 BigInt 这类独立实现来对照。

引擎清单的来源：直接运行 crosscalc_tests --list-functions（不依赖网络与 GUI），
也可以用 --functions-file 指定一个预先保存的清单文本。

用法：
  python3 tests/check_frontend.py --tests-binary build/Release/crosscalc_tests.exe
"""

import argparse
import re
import shutil
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

    # 响应字段也要用上：漏读 incomplete 的话，敲到一半的表达式就会一直弹红框
    for field, why in (
        ("data.ok", "求值是否成功"),
        ("data.display", "显示用字符串"),
        ("data.error", "错误消息"),
        ("data.error_pos", "错误位置"),
        ("data.incomplete", "“表达式还没输完”标记"),
    ):
        fail.check(field in js, f"app.js 未处理响应字段 {field}（{why}）")

    fail.info("接口与字段契约检查完成", verbose)


def css_rule(css, selector):
    """取出某个选择器的声明块文本（够用即可：前端 CSS 没有嵌套规则）。"""
    m = re.search(re.escape(selector) + r"\s*\{([^}]*)\}", css)
    return m.group(1) if m else None


def check_layout_anchors(fail, html, js, css, verbose):
    """守住"打字时键盘不许乱动"这条布局约定。

    它是纯 CSS 行为，C++ 单测和 JS 纯函数都碰不到；而回归起来极其显眼又极其
    容易被好心改坏（比如把固定高度换成 `hidden` 属性，或给键盘加个 flex-shrink
    好让窗口矮一点时"看起来更紧凑"）。所以在这里用静态检查钉住。
    """
    root = css_rule(css, ":root")
    fail.check(root is not None and "--error-slot" in root,
               "style.css 的 :root 里应当定义 --error-slot（错误提示区的固定高度）")

    box = css_rule(css, ".error-box")
    fail.check(box is not None and "height: var(--error-slot)" in box,
               ".error-box 的高度必须写死成 height: var(--error-slot)："
               "否则错误出现/消失时，下面的键盘会整体上下跳")

    empty = css_rule(css, ".error-box.is-empty")
    fail.check(empty is not None, "style.css 缺少 .error-box.is-empty 规则")
    if empty:
        fail.check("visibility: hidden" in empty,
                   ".error-box.is-empty 要用 visibility: hidden 藏内容")
        fail.check("display" not in empty,
                   ".error-box.is-empty 不能用 display: none / display 改动："
                   "元素高度会塌成 0，键盘照样跳")

    # 空错误框靠类名而不是 hidden 属性来隐藏
    tag = re.search(r'<div[^>]*id="error-box"[^>]*>', html)
    fail.check(tag is not None, "index.html 中找不到 #error-box")
    if tag:
        fail.check("is-empty" in tag.group(0),
                   "#error-box 初始状态就应带 is-empty 类（而不是 hidden 属性）")
        fail.check("hidden" not in tag.group(0),
                   "#error-box 不能用 hidden 属性：一藏就把固定高度也藏没了")

    fail.check("classList.add('is-empty')" in js and "classList.remove('is-empty')" in js,
               "app.js 应当用 .is-empty 类来显示/隐藏错误提示（不是 hidden 属性）")
    fail.check("errorBox.hidden" not in js,
               "app.js 不该再动 errorBox.hidden：一设就把错误区高度弄没了，键盘会跳")

    # 键盘自己：宁可让整列滚，也不能被 flex 压扁
    calc = css_rule(css, ".calc")
    fail.check(calc is not None and "overflow-y: auto" in calc,
               ".calc 需要 overflow-y: auto：窗口太矮时让整列滚动，"
               "否则 flex 会把键盘（唯一可压缩的一块）一直压到 0 高")

    keypads = css_rule(css, ".keypads")
    fail.check(keypads is not None, "style.css 缺少 .keypads 规则")
    if keypads:
        fail.check(re.search(r"flex:\s*1\s+0\s+auto", keypads) is not None,
                   ".keypads 必须是 flex: 1 0 auto（不许收缩）：空间不够时应当"
                   "撑出滚动条，而不是把按键挤成一排看不清的字")

    rows = css_rule(css, ".kp-row")
    fail.check(rows is not None and "min-height" in rows,
               ".kp-row 需要 min-height：行高被压扁后按键上的字符就看不清了")

    # 进制读数一种一行（hex / dec / oct / bin 各占一行）。
    # 曾经为了省 50px 高度把它们压成两行（hex/dec/oct 并排），结果四种读数互相
    # 抢宽度——bin 的 32 位串被挤得要左右滚。空间该从空白里省，不该从读数里省。
    base_list = css_rule(css, ".base-list")
    fail.check(base_list is not None, "style.css 缺少 .base-list 规则")
    if base_list:
        cols = re.search(r"grid-template-columns\s*:\s*([^;]+)", base_list)
        fail.check(cols is None or "repeat" not in cols.group(1),
                   f".base-list 不该再排成多列（现在是 {cols.group(1).strip() if cols else '?'}）："
                   "四种进制的读数要各占一行，否则位数长的读数会被挤得左右滚")
    fail.check(re.search(r'\.base-row\[data-base="bin"\]\s*\{[^}]*grid-column', css) is None,
               "不该再有 .base-row[data-base=bin] 的 grid-column 跨列规则："
               "那是两行时代的补丁，四行面板里没有意义")

    # 提示区不常驻"平台不支持 QWORD"之类的坏消息：用户改不了，天天挂着只会
    # 让人以为哪里坏了。QWORD 按钮该灰还是灰、原因挂在它的 title 上就够了。
    # 这里认的是当初那条常驻提示的原话（标题里的说明不算，那是悬停才看的）。
    stale = [s for s in ("本平台引擎是 double", "请用 BYTE/WORD/DWORD", "不支持 QWORD：")
             if s in js]
    fail.check(not stale,
               f"app.js 里又出现了常驻的平台限制提示 {stale}："
               "这类消息不该占着提示区（放按钮 title、把按钮置灰就够）")

    fail.info("布局约定检查完成（错误区固定高度 + 键盘不被压缩 + 四行进制读数）", verbose)


# ---------------------------------------------------------------------- 主流程 --
def run_js_unit_tests(fail, verbose):
    """用 Node 跑 app.js 里纯函数的单元测试（tests/js_frontend_test.js）。

    这些函数负责把用户敲的二进制/八进制数字重排成 0x 字面量。算错了不会有任何
    报错，只会得到一个不同的数字，靠眼睛看不出来，所以值得真的执行一遍。

    没装 Node 时返回 False 并说明原因 —— 这是唯一会让整体判失败的方式，
    因为"跳过"必须显式，不能悄悄算通过（CI 里一定有 Node，所以一定会跑到）。
    """
    print("\n[7] app.js 纯函数单元测试（Node）")
    node = shutil.which("node")
    if node is None:
        print("    x 未找到 node 可执行文件，无法运行 tests/js_frontend_test.js")
        print("      （CI 环境自带 Node；本地请安装 Node.js 后重跑）")
        fail.check(False, "缺少 node，无法运行 app.js 纯函数单元测试")
        return False

    script = REPO_ROOT / "tests" / "js_frontend_test.js"
    if not script.is_file():
        fail.check(False, f"缺少测试脚本 {script}")
        return False

    proc = subprocess.run([node, str(script)], capture_output=True, text=True,
                          encoding="utf-8", errors="replace")
    out = (proc.stdout or "").strip()
    if verbose or proc.returncode != 0:
        for line in out.splitlines():
            print("    " + line)
        if proc.stderr and proc.stderr.strip():
            for line in proc.stderr.strip().splitlines()[:10]:
                print("    ! " + line)

    passed = proc.returncode == 0 and "ALL JS TESTS PASSED" in out
    fail.check(passed, "app.js 纯函数单元测试未通过（node tests/js_frontend_test.js）")
    if passed:
        # 断言条数由脚本自己报，这里把它带进汇总行
        m = re.search(r"(\d+) 条断言", out)
        fail.info(f"    node: {m.group(1) if m else '?'} 条断言，全部通过", verbose)
    return passed


def check_key_labels(fail, js, verbose):
    """按键标签的硬约束：单个 0-9/A-F 字符只能是"数字键"本身。

    buildKey() 判断"这个键在当前进制下能不能用"时读的是 key.label——只要标签
    长得像十六进制数字，就会被送去和进制比较。所以任何**非数字**的键都不能用
    单个 0-9/A-F 当标签，否则它会在 HEX 以外的进制下被一起禁用。

    这条约束是踩出来的：清零键原来叫 `C`，于是被当成数字 12，在 DEC/OCT/BIN
    （以及 base 固定为 dec 的标准/科学模式）里全灰、点不动，用户只能来问
    "这个键是干嘛的"。现在清零键叫 `AC`。
    """
    pairs = re.findall(r"k\(\s*'([^']*)'\s*,\s*'([^']*)'", js)
    fail.check(len(pairs) > 50, f"app.js 里应当能扫出 50 个以上的按键定义（实际 {len(pairs)}）")

    bad = []
    for label, insert in pairs:
        if len(label) == 1 and label in "0123456789ABCDEF":
            # 合法的数字键：insert 就是它自己，或是十六进制数字键的 digitA..digitF
            if insert != label and insert != "digit" + label:
                bad.append(f"{label}（insert={insert}）")
    fail.check(not bad,
               f"这些键的标签是单个 0-9/A-F，却不是数字键 {bad}："
               "它们会被按键的进制可用性判断当成数字，在部分进制下被误禁用")

    # 反面：十六进制数字键必须老老实实用单个 A-F
    hexdigits = [lb for lb, ins in pairs if ins.startswith("digit")]
    fail.check(sorted(hexdigits) == list("ABCDEF"),
               f"十六进制数字键应当正好是 A-F 六个（实际 {sorted(hexdigits)}）")

    # 清零键叫 AC 不叫 C（原因见上）
    clears = [lb for lb, ins in pairs if ins == "clear"]
    fail.check(clears and all(lb == "AC" for lb in clears),
               f"清零键的标签应当是 AC（实际 {clears}）：用单个 C 会被当成十六进制数字 12")

    fail.info("按键标签约束检查完成（非数字键不得使用单个 0-9/A-F 标签）", verbose)


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
                     "build/Release/crosscalc_tests", "build/Debug/crosscalc_tests.exe",
                     "build-linux/crosscalc_tests"):
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
    check_layout_anchors(fail, html, js, css, args.verbose)
    check_key_labels(fail, js, args.verbose)

    ok = run_js_unit_tests(fail, args.verbose)

    print("\n" + "-" * 58)
    print(f"  前端一致性检查 {fail.checks} 条，失败 {fail.count} 条")
    print("-" * 58)
    if fail.count == 0 and ok:
        print("\nALL FRONTEND CHECKS PASSED")
        return 0
    print("\nFRONTEND CHECKS FAILED")
    return 1


if __name__ == "__main__":
    sys.exit(main())
