# crosscalc

[![build](https://github.com/leiddev/crosscalc/actions/workflows/build.yml/badge.svg)](https://github.com/leiddev/crosscalc/actions/workflows/build.yml)

一个跨平台的桌面计算器：**标准 / 科学 / 程序员**三种模式，**Windows / Linux** 均已实测。

界面外壳放在 [**webview-httplib-demo**](https://github.com/leiddev/webview-httplib-demo) 上
（webview 窗口 + cpp-httplib 本地服务 + cpp-embedlib 内嵌前端 + nlohmann/json），
**所有计算都交给 [tinyexpr-plusplus](https://github.com/Blake-Madden/tinyexpr-plusplus)** ——
本项目不自己实现任何计算函数，只负责三件事：

| 层次 | 归谁 |
|---|---|
| 表达式解析、优先级、括号、函数调用 | tinyexpr-plusplus |
| 全部数学运算、三角函数、位运算 | tinyexpr-plusplus |
| 模式的输入校验、错误信息中文化、结果的进制显示 | 本项目 |

界面是一个本地 HTML 页面，但它不是从磁盘读的——HTML / CSS / JS 全部被 `cpp-embedlib`
编译进可执行文件，由后台的 `cpp-httplib` 服务器从内存里发出来，窗口由 `webview` 创建
（Windows 内核是 Edge WebView2，Linux 是 GTK + WebKitGTK）。

```
┌──────────────────────────────────────────────┐
│  webview 窗口 (WebView2 / WebKitGTK)          │
│  ┌────────────────────────────────────────┐  │
│  │ index.html + app.js  ← 内存中的内嵌资源 │  │
│  │                                        │  │
│  │  按下等号 ──► window.cppEvaluate(expr)  │  │
│  │        │  webview::bind（同进程，最快） │  │
│  │        │                               │  │
│  │        └─► C++ 引擎（tinyexpr++）        │  │
│  │        （回退路径：POST /api/eval）      │  │
│  └────────────────────────────────────────┘  │
└──────────────────────────────────────────────┘
```

## 三种模式

| 模式 | 输入 | 说明 |
|---|---|---|
| **标准** | 表达式 | 四则、括号、`pi`/`e`、幂 `^`（`**` 同义）、取模 |
| **科学** | 表达式 | 三角函数 / 反三角 / 双曲 / 对数 / 阶乘 / 组合数等；**三角函数只支持弧度** |
| **程序员** | 数字键 + 表达式 | hex / dec / oct / bin 四种进制同时显示，字长 BYTE / WORD / DWORD / QWORD，按位运算函数 |

统一**表达式模式**：三种模式都是在同一个输入框里敲表达式，只是键盘按钮与结果面板不同。

支持的能力直接来自 tinyexpr++，随时可以在界面里查（「函数」按钮，或
`crosscalc_tests --list-functions`）。

## 下载预编译版本（不想自己编译）

到 [**Releases**](https://github.com/leiddev/crosscalc/releases) 下最新的那个：

| 文件 | 平台 | 运行前提 |
| --- | --- | --- |
| `crosscalc-vX.Y.Z-windows-x64.exe` | Windows 10/11 x64 | 双击即可。需要 **WebView2 运行时**（Win11 和较新的 Win10 一般自带） |
| `crosscalc-vX.Y.Z-linux-x64` | Linux x64 | `chmod +x` 后运行。需要 **WebKitGTK 4.1**（`libwebkit2gtk-4.1-0`）和一个可用的显示环境 |

二进制不是手工传上去的，而是**推 `v*` tag 时由 CI 现编现发**：

```
git tag -a v0.1.0 -m "..."   &&   git push origin v0.1.0
        │
        └─ .github/workflows/release.yml
             ├─ build   复用 build.yml：Windows + Ubuntu 22.04 各编一遍
             │           （两个平台都必须通过全部自测，见下）
             └─ publish download-artifact 取回产物 → gh release create
```

## 目录结构

```
crosscalc/
├─ CMakeLists.txt          构建脚本（依赖全部自动拉取）+ 测试注册
├─ CMakePresets.json       Windows: x64 / x64-console；Linux: linux / linux-debug
│                          另外带 windows-release / linux-release 两个自测预设
├─ src/
│  ├─ main.cpp             HTTP 服务 + 路由 + 窗口 + JS↔C++ 绑定
│  └─ core/                计算核心（唯一需要看懂的地方）
│     ├─ calc_types.h/.cpp     模式 / 进制 / 字长 / 请求与结果的类型
│     ├─ calc_engine.h/.cpp    包住 te_parser：校验、求值、错误归一化
│     ├─ error_text.h/.cpp     把库的英文错误翻成中文，并补上出错位置
│     └─ number_format.h/.cpp  显示层：进制、字长掩码、最短可还原浮点显示
├─ www/                    前端（会被编译进可执行文件）
│  ├─ index.html  style.css  app.js
├─ tests/
│  ├─ test_calc.cpp        单元测试（模式、错误、格式化、已知限制）
│  ├─ vector_runner.cpp    黄金向量驱动器（读 JSON）
│  ├─ vectors/*.json       324 条黄金向量（standard/scientific/programmer/errors）
│  ├─ e2e_api.py           端到端：真实进程 + 全部向量经 HTTP 回放
│  ├─ check_frontend.py    前端 ↔ 引擎一致性（含 DOM id、函数名、/api 契约）
│  ├─ js_frontend_test.js  app.js 纯函数单元测试（Node，与 BigInt 对照）
│  └─ smoke_gui.sh         Linux GUI 冒烟测试（Xvfb，验证渲染链路）
├─ docs/design-decisions.md  为什么这么做（以及为什么不那么做）
├─ .github/workflows/      CI：两个平台各自跑完全部自测
└─ THIRD_PARTY_NOTICES.md  三方组件与许可
```

## 构建与运行

### Windows（Visual Studio 2022 或更新）

需要：Visual Studio 2022 或更新（含「使用 C++ 的桌面开发」工作负载）、CMake ≥ 3.20、Git、
能访问 github.com（首次配置还要访问 nuget.org）。
预设**不指定 Visual Studio 版本**，会跟随本机默认（最新的）VS，所以 VS2022 / VS2026 都能直接用（见坑 10）。
运行环境需要 **WebView2 运行时**（Win10/11 一般已随 Edge 预装）。

```powershell
cd C:\Project\crosscalc

cmake --preset x64              # 配置（首次会拉取依赖 + 下载 WebView2 SDK，需要几分钟）
cmake --build --preset release
.\build\Release\crosscalc.exe
```

### Linux

需要：**CMake ≥ 3.20**、**GCC ≥ 10**（或等价的 clang）、`pkg-config`、
GTK3 + WebKitGTK 的开发包、Git、能访问 github.com。
无显示器的机器（容器 / CI / 服务器）想真的把窗口跑起来，还要 `xvfb`。

```bash
# Ubuntu 22.04 / 24.04
sudo apt install -y build-essential cmake ninja-build pkg-config \
                    libgtk-3-dev libwebkit2gtk-4.1-dev xvfb

cmake --preset linux
cmake --build --preset linux-release
./build-linux/crosscalc              # 无显示器时见下
```

无显示器时（`DISPLAY` 为空）：

```bash
xvfb-run -a --server-args="-screen 0 1280x1024x24" ./build-linux/crosscalc --port 8080
```

`tests/smoke_gui.sh` 已经把上面这套环境变量、端口探测和无头启动都封好了，也可以直接用来跑自测。

### 可用预设

| 预设 | 作用 |
|---|---|
| `x64` / `release`、`debug` | Windows 默认 GUI 版，输出在 `build\Release` |
| `x64-console` / `console-release` | 控制台版（带日志窗口），输出在 `build-console\Release` |
| `linux` / `linux-release` | Linux（Ninja + Release），输出在 `build-linux/` |
| `windows-release` / `linux-release`（test） | 自测预设：`ctest --preset windows-release` |

预设带 `condition`，在 Linux 上 `cmake --list-presets` 只会列出 `linux*`，反之亦然——不会选错。

> Linux 上默认构建本来就是带 stdout 的普通程序，日志直接打在终端里，
> **不需要** `CROSSCALC_CONSOLE`（那个开关只影响 Windows 的 GUI 子系统）。

可选参数：

| 参数 | 作用 |
|---|---|
| `--port 8080` | 用固定端口（默认自动挑一个空闲端口） |
| `--headless` | 只起 HTTP 服务、不开窗口（自测脚本用的就是这个） |
| `--debug` | 更详细的日志 |
| 构建时 `-DCROSSCALC_CONSOLE=ON` | 仅 Windows：编译成控制台程序，能直接看到日志 |

## 自测怎么跑

**CI 会把这四层全部跑绿才算通过**，本地也可以逐层复现：

```powershell
# ① 单元测试 + 黄金向量（两个 ctest 测试，都要求打印 ALL TESTS PASSED）
ctest --preset windows-release

# ② 端到端：以 --headless 启动真实进程，把全部向量经 HTTP 回放一遍
python tests\e2e_api.py --binary build\Release\crosscalc.exe

# ③ 前端 ↔ 引擎一致性：键盘按钮只能插入引擎真正认识的函数
python tests\check_frontend.py --tests-binary build\Release\crosscalc_tests.exe

# ④ app.js 纯函数（与 BigInt 对照）
node tests\js_frontend_test.js
```

Linux 上多一层 GUI 冒烟（真的把窗口跑起来，验证 WebKitGTK 渲染链路）：

```bash
bash tests/smoke_gui.sh build-linux/crosscalc
```

各层分工、以及「为什么值得加第 ③ ④ 层」，见
[docs/design-decisions.md 第 9 条](docs/design-decisions.md)。

## 运算能力与已知限制

都是**有意为之**，不是没做完；细节与理由见 [docs/design-decisions.md](docs/design-decisions.md)。

| 项 | 说明 |
|---|---|
| `^` 是**乘方**（`2^10 = 1024`），`**` 同义 | 位运算用 `BITAND()` / `BITOR()` / `BITXOR()` / `BITNOT()` |
| `&` 和 `\|` 是**逻辑**运算 | 它们始终是 `&&` / `||` 的简写，不是按位与/或 |
| 三角函数**只有弧度** | 不支持 DEG / GRAD |
| 没有 `0b` / `0o` 字面量 | 十六进制写 `0x`；程序员模式的二进制/八进制数字键会自动换算成 `0x` |
| `BIN2DEC()` / `OCT2DEC()` / `HEX2DEC()` 最多 10 位数字 | 且凑满 10 位时按**有符号**解释（`BIN2DEC("1111111111")` 是 −1）。输入长数值请用 `0x` 字面量 |
| `log2` / `sec` / `csc` / `asinh` / `gcd` 等不支持 | 库不提供 |
| 位运算参数上限 2^48−1 | 库限制，超出会给出明确提示 |
| **QWORD(64 位字长) 仅在 Linux / macOS(x86-64) 可用** | MSVC 的 `long double` 与 `double` 同宽，Windows 上会明确拒绝而不是给出被舍入的答案 |
| 浮点结果显示最多 17 位有效数字 | 两个平台显示一致；需要精确的 64 位数值请用程序员模式看位模式 |

## 接口一览

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/`、`/style.css`、`/app.js` | 内嵌前端资源（MIME 自动识别） |
| POST | `/api/eval` | **求值主接口**，body 是 JSON：`{"expression":"...","mode":"programmer","base":"hex","wordsize":"32"}` |
| GET | `/api/eval?expr=..&mode=..&base=..&wordsize=..` | 同上，便于命令行调试 |
| GET | `/api/info` | 各库版本（tinyexpr-plusplus / cpp-httplib / webview / cpp-embedlib / nlohmann-json）、OS、PID、运行时长 |
| GET | `/api/platform` | `supports_64bit`、`max_integer_bitness`、`max_bitops_value` —— 前端据此禁用 QWORD 按钮 |
| GET | `/api/functions` | 引擎支持的全部函数与常量 |
| GET | `/api/assets` | 列出被 cpp-embedlib 内嵌的文件 |
| GET | `/api/health` | 存活性探测（自测脚本用） |

`/api/eval` 的返回里，程序员模式会额外带一个 **`all_bases`** 字段（hex/dec/oct/bin 四种显示），
这样进制换算只有后端一份实现，前端不做任何位运算。

失败时还会带一个 **`incomplete`** 字段：`true` 表示这次失败只是"表达式还没输完"
（`2+`、`sin(`、手打 `sqrt` 打到一半……），不是用户写错了。
前端实时预览时据此把错误先按住（结果区显示灰色的 `—`），只在按下等号/回车时才弹红框—— 
否则每敲一个键都会闪一次错误。判定规则见 [设计决策 §14](docs/design-decisions.md)。

错误框本身占固定高度（`--error-slot`），出现和消失都不会推动下面的键盘；
键盘在窗口变矮时也不会被压扁，见 [设计决策 §15](docs/design-decisions.md)。

JS → C++ 绑定：`cppEvaluate(requestJson)`、`cppCloseWindow()`。界面优先走
`cppEvaluate`（同进程、无 HTTP 往返），不可用时自动回退到 `POST /api/eval`。

```console
$ curl -s "http://127.0.0.1:18080/api/eval?expr=255&mode=programmer&base=hex"
{"ok":true,"mode":"programmer","base":"hex","wordsize":32,"expression":"255","display":"FF",
 "value":255.0,"integral":true,"error":null,"error_pos":-1,"error_len":0,"incomplete":false,
 "all_bases":{"hex":"FF","dec":"255","oct":"377","bin":"1111 1111"}}

$ curl -s "http://127.0.0.1:18080/api/eval?expr=2%2B"
{"ok":false,...,"error":"表达式不完整：运算符 '+' 后面缺少操作数（位置 2）。",
 "error_pos":1,"error_len":1,"incomplete":true}   # 还在输入，前端先不弹错误
```

## 需要注意的几个坑（都实测踩过）

外壳部分（坑 1–12）来自 [webview-httplib-demo](https://github.com/leiddev/webview-httplib-demo)，
换成计算器后又踩到 13–23。

1. **webview 0.12 里 C API 和 C++ API 是并存的，别信“C++ API 已被移除”的说法。**
   `core/include/webview/webview.h` 里**搜不到 `class webview`**，
   但公开类型 `webview::webview` 是个 **type alias**（`using webview = browser_engine;`），
   真正的实现在 `webview::detail::engine_base` 与三个平台子类。
   教训：**判断某个 API 是否还在，不能只搜 `class X`**——别名和宏同样可能是入口。

2. **webview 必须在 UI 线程调用（C API / C++ API 都一样）。**
   `eval` / `terminate` 在子线程里调用会“返回成功但毫无效果”。子线程要操作窗口，
   用 `w.dispatch(fn)` 投递到主线程；`w.bind()` 注册的回调本身就在主线程执行。

3. **`w.init(js)` 注入的脚本只对“之后创建”的文档生效**，必须在 `w.navigate()` **之前**调用。

4. **首次配置要联网。** 依赖走 `FetchContent`；Windows 上 webview 还会自动从 nuget.org
   拉 `Microsoft.Web.WebView2` SDK。nuget 不可达时可以手动指定
   `-DMSWebView2_ROOT=<解压目录>`。默认启用内置 WebView2Loader，所以**不需要**附带 `WebView2Loader.dll`。

5. **`WebAssets.h` 是构建时生成的**，第一次编译前 IDE 会标红，`cmake --build` 一次之后就正常。

6. **（仅 Windows）无控制台窗口的 GUI 程序看不到 `printf`。** 日志走 `OutputDebugStringW`；
   调试用 `-DCROSSCALC_CONSOLE=ON`。GUI 子系统下 MSVC 默认找 `WinMain`，
   本项目用 `target_link_options(... "/ENTRY:mainCRTStartup")` 保留标准 `main()`。

7. **端口冲突**：默认 `bind_to_any_port` 自动挑空闲端口。但**自测脚本必须自己挑一个空闲端口**
   ——写死端口时，上一次跑剩下的进程会继续占着它，于是 curl 打到的是**上一个进程**，
   报出来的错却看着像新进程的问题（实测被一个 404 误导过）。
   `smoke_gui.sh` 因此还做了两件事：启动前确认端口是空的；启动后从 `/api/info` 读回 PID
   确认应答的确实是刚启动的那个进程。

8. **Linux 上的三个硬门槛**（缺一个都编不过）：
   **CMake ≥ 3.20**（20.04 自带的 3.16 会拒绝配置）、
   **GCC ≥ 10**（GCC 9 的 libstdc++ 没有 `<span>`，cpp-embedlib 会报错）、
   **`pkg-config` + GTK3/WebKitGTK 开发包**（webview 在 configure 阶段就
   `find_package(PkgConfig REQUIRED)`）。无头机器还需要 `xvfb`。

9. **原生句柄不是同一个东西。** `w.window()` 在 Windows 上返回 `HWND`，
   Linux 上返回 `GtkWidget *`。

10. **别在预设里写死 Visual Studio 版本。** `windows-latest` 现在只剩 VS2026，
    写死 `"Visual Studio 17 2022"` 会直接报 `could not find any instance`。

11. **nlohmann/json 的三个坑**：默认 `json` 会把键**按字母序**输出（要 `ordered_json`）；
    `dump()` 遇到非法 UTF-8 会抛 `type_error.316`（要 `error_handler_t::replace`）；
    `double` 一律输出成 `42.0`（不是 bug，「库不替你猜」）。

12. **开了 `/utf-8` 之后，Win32 的 `*A` 接口全是坏的，`*W` 接口必须自己转码。**
    `MessageBoxA` 会把 UTF-8 当 ANSI 码页解释；而
    `std::wstring(msg.begin(), msg.end())` 是**按字节**往 `wchar_t` 里塞，
    中文会变成乱码（`/W3` 下没有任何警告）。修法是 `to_wstring_utf8()`
    （`MultiByteToWideChar(CP_UTF8, ...)`，`flags` 传 0 让坏字节变 U+FFFD）。
    **报错信息本身不能因为混进一个坏字节就报不出来**——这条路径正好是第一次用的人最容易撞上的那条。

13. **MSVC 必须加 `/Zc:__cplusplus`，否则 8 个旋转函数会被静默编译掉。**
    MSVC 默认把 `__cplusplus` 报成 `199711L`，而 tinyexpr++ 里
    `BITLROTATE8/16/32/64`、`BITRROTATE8/16/32/64` 用
    `#if __cplusplus >= 202002L` 保护——不加这个开关**不报错、不警告**，
    只是按钮按下去提示「未知函数」。这条是「编译得过、测试才发现」的典型。

14. **`TE_BITWISE_OPERATORS` 不要打开，而且 `&` / `|` 永远是逻辑运算符。**
    打开它 `^` 会变回按位异或，科学模式里 `2^10` 就不再是 1024。
    更容易误解的是：**`&` 和 `|` 不受这个宏影响**，它们始终是 `&&` / `||` 的简写。

15. **64 位整数（QWORD）要显式定义 `TE_LONG_DOUBLE`，而且必须是 PUBLIC 编译定义。**
    `te_parser::supports_64bit()` 的实现就是 `numeric_limits<te_type>::digits >= 64`，
    而 `te_type` 默认是 `double`（53 位）。定义后 `te_type` 变 `long double`
    （x86-64 上是 80 位扩展精度），8/16/32/64 位字长全都精确。
    三件事必须一起做对：
    - **判断条件**用 `check_cxx_source_compiles` 试编译，
      **不要用 `if(MSVC)` 或 `CMAKE_CXX_SIZEOF_LONG_DOUBLE`**：后者在部分 CMake
      版本/平台上根本不存在，实测在 Ubuntu 22.04 + CMake 3.22 上取到空值，
      于是条件静默为假、QWORD 悄悄退化成 53 位（不报错）。用
      `numeric_limits<long double>::digits >= 64` 试编译，口径和库完全一致。
    - **必须是 `PUBLIC` 定义**：`te_type` 出现在 `te_parser` 的公开接口上，
      只有 tinyexpr.cpp 带这个宏、而 calc_engine.cpp 不带，就是 ODR 违反 +
      调用约定错乱。
    - **全链路不许窄化成 `double`**。这条最隐蔽：`calc_engine.cpp` 里曾经有一行
      `const double value = static_cast<double>(raw);`，于是
      `0xFFFFFFFFFFFFFFFF` 在进入显示层之前就被舍入成 2^64，
      程序员模式显示 `18446744073709551616` 而不是 `FFFFFFFFFFFFFFFF`。
      现在全程用 `num_t`（见 `src/core/calc_types.h`），且有
      `static_assert` 挡住把 `num_t` 改窄的改动。

16. **显示层统一收敛到 double 精度（17 位有效数字），这是有意的。**
    按 80 位精度如实展开会让 `0.1+0.2` 显示成 `0.300000000000000000011`，
    信息量没增加、可读性全没了，而且两个平台显示还会不一样。
    代价是**少数用例在两个平台上的正确答案本来就不同**，例如：

    | 表达式 | double 平台 | 扩展精度平台 |
    |---|---|---|
    | `0.1+0.2` | `0.30000000000000004` | `0.3` |
    | `cot(pi/4)` | `1.0000000000000002` | `1` |
    | `1e999` | 溢出报错 | 正常求值（没超出 long double 上限） |

    这类用例在向量里用 **`"arith": "double"` / `"arith": "extended"`** 标明适用平台，
    在另一个平台上自动**跳过**（C++ 与 Python 两侧的门控逻辑必须一致，
    否则会出现「一边跳过、一边判失败」）。`1e9999` 则两者都溢出，不需要标注。

17. **`HEX2DEC` / `BIN2DEC` / `OCT2DEC` 不是通用的进制转换函数**，别拿它们当输入通道：
    参数**最长 10 个字符**（超了直接返回 NaN，不抛异常），且**凑满 10 位时按有符号解释**：
    `bin` 是 10 位、`oct` 是 30 位、`hex` 是 40 位。所以
    `BIN2DEC("1111111111")` 是 **−1** 而不是 1023。
    程序员模式的二进制/八进制数字键因此**不用** `BIN2DEC` / `OCT2DEC`，
    而是把数字按 4 位一组**无损重排成 `0x` 字面量**（纯查表 + 字符串拼接，无算术）。

18. **tinyexpr++ 有三种互不相同的失败方式**，只写一个 `if` 一定会漏：
    ① 抛 `std::runtime_error`（除零、负数开方、位运算参数越界）；
    ② `success() == false` 但 **`get_last_error_message()` 是空字符串**（语法/未知符号，只给位置，且位置是 **0 基**，`te_parser::npos == -1` 表示无位置）；
    ③ **静默返回 NaN 而 `success()` 仍为 `true`**（例如 `HEX2DEC("ZZ")`）。
    第 ③ 种如果不处理，界面会显示一个 `NaN` 而没有任何提示。
    `Engine::evaluate()` 的约定是**永不抛异常**，调用方不必写 try/catch。

19. **两个小坑**：
    - **GCC 11 的 `-Wstringop-overflow` 误报**：`"字面量" + std::string` 拼串在
      `-O3` 下会报 `char_traits.h: writing N bytes into a region of size M`
      （SSO 缓冲被误判）。改成「先建 `std::string` 再 `+=`」就没有了。CI 用的正是 GCC 11。
    - **`subprocess.PIPE` 会死锁**：自测脚本里程序每处理一个请求都写日志，
      管道 64KB 缓冲写满后子进程阻塞在写日志上，整个测试挂死（实测挂满 300 秒）。
      输出要重定向到临时文件。
    - **app.js 刻意避开 ES2020 语法**（`??`、`?.`）：Ubuntu 22.04 自带的 Node 12
      加载它跑单元测试时会直接语法报错，而 `??` 换掉的代价几乎为零。

20. **实时求值意味着"敲到一半"也必须能被识别**：输入框每变一次就求值一次（90 ms 防抖），
    于是 `2+`、`sin(`、手打 `sqrt` 打到 `sq` 的**每一拍都是失败**。
    把这些失败按错误显示出来，用户打字全程都在看红框——那不是提示，是噪音。
    解决办法不是让前端去猜，而是后端在 `EvalResult` 上给一个 **`incomplete` 标记**
    （由 `diagnose_syntax_error()` 判定，见 `src/core/error_text.cpp`），
    前端实时预览时先把这些错误按住，只在按下等号/回车时才弹。
    什么算"没输完"、什么算"写错了"，以及"数字开头的 token 不享受这个待遇"这类边界，
    都写在[设计决策 §14](docs/design-decisions.md) 里。

21. **错误提示区的高度必须写死，键盘才不会被顶着走**：错误框平时用
    `.error-box.is-empty { visibility: hidden }` 藏内容，**不是** `hidden` 属性/`display: none`——
    后者会把元素高度塌成 0，错误一来一去整块键盘就上下跳 40px，而打字全程都在出错和恢复正常。
    高度用 `--error-slot: 80px` 写死，值是量出来的（两行提示 + 两行 `^` 定位 = 76px），
    再长的提示就在框内自己滚。同理，表达式太长时 `^` 定位行会自己横滚、把箭头滚没了，
    所以 `app.js` 的 `caretWindow()` 只截一小段（两头 `…`）。
    另外 `.keypads` 是 `flex: 1 0 auto`——窗口矮下去时宁可让 `.calc` 出滚动条，
    也不能让 flex 把键盘压成 0 高（程序员模式 6 行最先撞上）。
    程序员模式的高度是三种东西抢出来的：6 行键盘、**四行**进制读数（一种进制一行，
    读数之间不抢宽度）、默认 1180×820 的窗口。挤不下时该从空白里省（`.calc`/`.keypads`
    间距、`.display`/`.base-row` 内边距），**不该缩字号**，也不该把四行读数压回两行。
    这几条都是纯 CSS 约定、单测碰不到，所以由 `tests/check_frontend.py` 静态钉住。
    细节见[设计决策 §15](docs/design-decisions.md)。

22. **前端是编译进 exe 的，改完 `www/` 必须重新编译**：`www/` 里的三个文件由
    cpp-embedlib 在**构建期**转成 `_data_*.cpp` 再链进 `crosscalc.exe`，运行时
    直接从内存里读。所以只改 `www/`、不跑 `cmake --build`，你打开的 exe 里还是
    旧界面——"改了没反应"十有八九是这个，而不是代码没生效。
    更坑的是**验证方式**：`ctest`/`e2e_api.py` 都不碰前端，而用来量布局的
    headless 脚本是从**磁盘**上的 `www/` 取页面的，它永远不会替你发现 exe 里的
    那份是旧的。要确认 exe 真的更新了，就起 `--headless` 后 `GET /app.js`
    跟磁盘上的文件比一比（一致才算数）。见[设计决策 §16](docs/design-decisions.md)。

23. **按键的上进制判断是按【标签】做的，标签不能撞上数字表**：`buildKey()` 里
    "这个键在当前进制下能不能用"读的是 `key.label`——只要标签长得像单个
    `0-9`/`A-F`，就会被当成十六进制数字送去和进制比较。清零键原来叫 `C`，
    于是被当成数字 12，在 `DEC/OCT/BIN`（以及 `base` 固定为 `dec` 的标准/科学模式）
    下全灰、点不动，用户只能来问"这个键是干嘛的"。现在清零键叫 **`AC`**：
    两个字符天然不参与这个判断，顺带也把程序员模式里"十六进制数字 `C`"和
    "清零 `C`"两个同名键的歧义去掉了。
    这条约束由 `tests/check_frontend.py` 的 `check_key_labels()` 守着
    （任何非数字键用单个 `0-9/A-F` 当标签都会报错）。见[设计决策 §17](docs/design-decisions.md)。

## 如何发版

完全「推 tag 触发」：不需要手工编译，也不需要往网页上拖任何文件。

1. **确认 `main` 上的 CI 是绿的**（看徽章）。发布用的二进制是 CI 从这个 tag 现场编出来的，
   而 CI 会跑完全部四层自测，所以绿 = 发出去的就是验证过的那份。
2. **定版本号**（SemVer，`v` 前缀不能少，工作流按 `v*` 匹配）：
   `v0.1.1`（修 bug）、`v0.2.0`（加功能）、`v1.0.0`（破坏性改动）、
   `v0.2.0-rc1`（自动标成 Pre-release，不顶掉 Latest）。
3. **写发布说明**：`release-notes/vX.Y.Z.md`，纯 Markdown，会**原样**成为 Release 正文。
   没有则退回根目录 `RELEASE_NOTES.md`，再没有才用自动生成（而自动生成列的是「合并的 PR」，
   本仓库都是直接 push，所以只会给一行 changelog 链接，建议手写）。
4. **提交 → 推 main → 打 tag**：

   ```bash
   git add release-notes/v0.2.0.md          # 说明文件要先提交：publish 读的是 tag 里的内容
   git commit -m "docs: 补上 v0.2.0 的发布说明"
   git push origin main

   git tag -a v0.2.0 -m "v0.2.0 - 一句话概括这次发了什么"
   git push origin v0.2.0
   ```

5. **看 Actions → Releases**：大约几分钟后出现标题 = tag 名、正文 = 第 3 步的说明、
   附件 = `crosscalc-<tag>-windows-x64.exe` 和 `crosscalc-<tag>-linux-x64`。

### 出问题了怎么办

- **publish 失败**：不会留下半成品。看失败那步的 annotation，修完删 tag 重推即可：
  `git tag -d v0.2.0 && git push origin --delete v0.2.0`。
- **只想改正文或换附件**：网页上 Releases → 该版本 → ✏️ Edit release，**不用**重新发版。
- **想彻底重发**：先在网页删掉 Release，再删 tag 重推（反过来会让 Release 变成没有 tag 的孤儿）。
- **已经发出去的 tag 不要改指向**：老下载链接会指向新的提交内容，和已发出的二进制对不上。

### 两个容易踩的坑

1. **别给 `build.yml` 的 push 触发再加 `tags`**：tag 只由 `release.yml` 触发，它再
   `workflow_call` 复用 build.yml。两边都写，同一次 tag 会跑两遍全量构建。
2. **tag 名会被拼进附件名**，且必须能以 `v*` 匹配上。

## 已验证的环境

**Windows**：Windows 10 22H2 x64 · VS2022 Community · MSVC 14.4x · CMake 3.31+
`build\Release\crosscalc.exe` 本机 **766,976 字节**，CI（`windows-latest`）产物 **772,096 字节**（均为编译 0 警告）

**Linux**：Ubuntu 22.04.1 LTS（无显示器）· GCC 11.4.0 · CMake 3.22.1 · Ninja ·
WebKitGTK 2.50.4（`webkit2gtk-4.1`）+ GTK 3.24.33 · Node 12 · Xvfb
`build-linux/crosscalc` 本机 **1,730,328 字节**，CI（`ubuntu-22.04`）产物 **1,751,152 字节**（均为编译 0 警告）

依赖版本：**tinyexpr-plusplus `404688c`** · cpp-httplib v0.38.0 · webview 0.12.0 ·
cpp-embedlib main · nlohmann/json v3.12.0 · Microsoft.Web.WebView2 1.0.1150.38

### 自测实测结果

| 层 | Windows | Linux |
|---|---|---|
| ① 单元测试 | 44 用例 / 265 断言 | 44 用例 / 266 断言 |
| ① 黄金向量（共 324 条） | 执行 318 条（跳过 6 条）/ 635 断言 | 执行 322 条（跳过 2 条）/ 643 断言 |
| ② HTTP 端到端（同一批向量经真实进程回放） | 1050 断言 | 1062 断言 |
| ③ 前端一致性 | 通过（含 Node 实跑） | 同 |
| ④ app.js 纯函数 | 330 断言（300 条与 BigInt 对照） | 同 |
| ⑤ GUI 冒烟（Xvfb） | —（无桌面会话） | 通过，日志里可见窗口自己拉的 `/app.js` |

**两边跳过条数不同是真实的、也正是设计目标**：324 条里 5 条打 `requires_64bit`、1 条打
`arith: "extended"`、2 条打 `arith: "double"`。Windows 只跳过前 6 条（并在结果里给出明确提示），
Linux 只跳过最后 2 条 —— 也就是说 64 位整数在 Linux 上确实全部执行且结果精确。
差异完全来自向量数据里的字段声明，C++（`vector_runner.cpp`）与 Python（`e2e_api.py`）两侧的
闸门写法刻意保持一致，不存在“某一边偷偷放水”的可能。
这正是 [docs/design-decisions.md 第 3 条](docs/design-decisions.md) 选定的行为。

### GitHub Actions

| 镜像 | 系统 | CMake | Visual Studio |
|---|---|---|---|
| `windows-latest` | Windows Server 2025 | 4.4.3 | **Enterprise 2026**（没有 VS2022） |
| `ubuntu-22.04` | Ubuntu 22.04 | 系统 3.22 / 镜像自带的更新 | — |

两个平台都必须跑完 ①–④（Linux 还有 ⑤）才算成功。

## 许可

MIT，见 [LICENSE](LICENSE)。三方组件与许可见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
（其中 tinyexpr-plusplus 为 zlib 许可，原始 tinyexpr 与 tinyexpr++ 的版权声明都已保留）。
