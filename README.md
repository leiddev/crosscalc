# webview-httplib-demo

[![build](https://github.com/leiddev/webview-httplib-demo/actions/workflows/build.yml/badge.svg)](https://github.com/leiddev/webview-httplib-demo/actions/workflows/build.yml)

用 **cpp-httplib + webview + cpp-embedlib** 搭的最小桌面应用示例（JSON 的序列化 / 解析交给
**nlohmann/json**），**Windows / Linux** 均已实测，macOS 理论可行但未验证。

跑起来之后是一个原生窗口，界面是一个本地 HTML 页面，但它不是从磁盘读的——
HTML / CSS / JS 全部被 `cpp-embedlib` 编译进了可执行文件，由后台的 `cpp-httplib` 服务器
从内存里发出来，窗口本身由 `webview` 创建（Windows 上内核是 Edge WebView2，Linux 上是 GTK + WebKitGTK）。

webview 用的是 **0.12.0 的 C++ API**（`webview::webview w(true, nullptr)` 加
`set_title` / `set_size` / `bind` / `navigate` / `run`，靠 RAII 析构销毁窗口）；
同一个头文件里 C API 依然存在，两者的对应关系见下面「C API ↔ C++ API 对照」。

```
┌──────────────────────────────────────────────┐
│  webview 窗口 (WebView2 / WebKitGTK)          │
│  ┌────────────────────────────────────────┐  │
│  │ index.html + app.js  ← 内存中的内嵌资源 │  │
│  │                                        │  │
│  │  fetch('/api/xxx')                     │  │
│  │        │                               │  │
│  │        ▼   HTTP (127.0.0.1:随机端口)    │  │
│  │  ┌──────────────────────────┐          │  │
│  │  │ cpp-httplib 服务器(线程)  │          │  │
│  │  └──────────────────────────┘          │  │
│  │                                        │  │
│  │  window.cppNativeXxx()                 │  │
│  │        │  webview::bind（不走 HTTP）    │  │
│  │        ▼                               │  │
│  │  C++ 函数（同一进程，主线程）            │  │
│  └────────────────────────────────────────┘  │
└──────────────────────────────────────────────┘
```

## 下载预编译版本（不想自己编译）

到 [**Releases**](https://github.com/leiddev/webview-httplib-demo/releases) 下最新的那个：

| 文件 | 平台 | 运行前提 |
| --- | --- | --- |
| `webview-demo-vX.Y.Z-windows-x64.exe` | Windows 10/11 x64 | 双击即可。需要 **WebView2 运行时**（Win11 和较新的 Win10 一般自带） |
| `webview-demo-vX.Y.Z-linux-x64` | Linux x64 | `chmod +x` 后运行。需要 **WebKitGTK 4.1**（`libwebkit2gtk-4.1-0`）和一个可用的显示环境 |

二进制不是手工传上去的，而是**推 `v*` tag 时由 CI 现编现发**：

```
git tag -a v0.1.1 -m "..."   &&   git push origin v0.1.1
        │
        └─ .github/workflows/release.yml
             ├─ build   复用 build.yml：Windows(VS2026) + Ubuntu 22.04 各编一遍（同一 run）
             └─ publish download-artifact 取回产物 → gh release create（用 CI 自带的 GITHUB_TOKEN，不需要 PAT）
```

发布说明的取用顺序（publish 里做了 checkout，读的就是这个 tag 的内容）：

1. `release-notes/<tag>.md`，例如 `release-notes/v0.1.1.md` —— 想手写就放这个；
2. 根目录 `RELEASE_NOTES.md`；
3. 都没有才回退到 `gh release create --generate-notes`。注意它列的是**合并的 PR**，
   而本仓库的提交都是直接 push，所以那时候正文只会有一行 changelog 链接。

tag 名里带连字符的（如 `v0.1.1-rc1`）会自动标成 **Pre-release**，不会顶掉 Latest release。

维护者要发新版本时，具体步骤和出问题怎么回滚见下面的 [**如何发版**](#如何发版)。

## 目录结构

```
webview-httplib-demo/
├─ CMakeLists.txt          构建脚本（四个依赖全部自动拉取：三个 git clone + 一个 url 下载）
├─ CMakePresets.json       Windows: x64 / x64-console；Linux: linux / linux-debug
├─ src/main.cpp            全部 C++ 代码：HTTP 服务 + 路由 + 原生绑定 + 窗口
├─ www/                    前端（会被编译进可执行文件）
│  ├─ index.html
│  ├─ style.css
│  └─ app.js
├─ .github/workflows/      CI：windows-latest + ubuntu-22.04 配置 + 编译（+ Linux 冒烟测试）
├─ .clang-format           C++ / JS 格式化规则（clang-format 19 实测 0 违规）
├─ .editorconfig           缩进、换行、编码统一
├─ .gitignore              build/、.vs/、MSVC 中间产物…
├─ .gitattributes          换行统一 LF、二进制标记、语言统计
├─ THIRD_PARTY_NOTICES.md  三方组件与许可（cpp-httplib/webview/cpp-embedlib/WebView2）
└─ LICENSE                 MIT
```

## 构建与运行

### Windows（Visual Studio 2022 或更新）

需要：Visual Studio 2022 或更新（含"使用 C++ 的桌面开发"工作负载）、CMake ≥ 3.20、Git、
能访问 github.com（首次配置还要访问 nuget.org）。
预设**不指定 Visual Studio 版本**，会跟随本机默认（最新）的 VS，所以 VS2022 / VS2026 都能直接用（见坑 10）。
运行环境需要 **WebView2 运行时**（Win10/11 一般已随 Edge 预装）。

```powershell
cd C:\Project\webview-httplib-demo

cmake --preset x64          # 配置（首次会拉取四个依赖库 + 下载 WebView2 SDK，需要几分钟）
cmake --build --preset release
.\build\Release\webview-demo.exe
```

可用预设：

| 预设 | 作用 |
|---|---|
| `x64` / `release`、`debug` | 默认 GUI 版，输出在 `build\Release` 或 `build\Debug` |
| `x64-console` / `console-release` | 控制台版（带日志窗口），输出在 `build-console\Release` |

预设带 `condition`，在 Linux 上 `cmake --list-presets` 只会列出 `linux*`，反之亦然——不会选错。

### Linux

需要：**CMake ≥ 3.20**、**GCC ≥ 10**（或等价的 clang）、`pkg-config`、
GTK3 + WebKitGTK 的开发包、Git、能访问 github.com。
无显示器的机器（容器 / CI / 服务器）想真的把窗口跑起来，还要 `xvfb`。

```bash
# Ubuntu 22.04 / 24.04
sudo apt install -y build-essential cmake ninja-build pkg-config \
                    libgtk-3-dev libwebkit2gtk-4.1-dev

# Ubuntu 20.04 只有 WebKitGTK 4.0，而且自带 GCC 9 编不了 cpp-embedlib（缺 <span>），
# 所以要额外装 g++-10 并指定 WEBVIEW_WEBKITGTK_API：
#   sudo apt install -y g++-10 libwebkit2gtk-4.0-dev
#   然后配置时加 -DCMAKE_CXX_COMPILER=g++-10 -DWEBVIEW_WEBKITGTK_API=4.0

cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux
./build-linux/webview-demo              # 或者用预设：cmake --preset linux && cmake --build --preset linux-release
```

无显示器时（`DISPLAY` 为空）：

```bash
export WEBKIT_DISABLE_DMABUF_RENDERER=1   # 虚拟机/无 GPU 时 DMA-BUF 会失败
export WEBKIT_DISABLE_COMPOSITING_MODE=1
export LIBGL_ALWAYS_SOFTWARE=1
xvfb-run -a --server-args="-screen 0 1280x1024x24" ./build-linux/webview-demo --port 8080
```

> Linux 上默认构建本来就是带 stdout 的普通程序，日志直接打在终端里，
> **不需要** `WEBVIEW_DEMO_CONSOLE`（那个开关只影响 Windows 的 GUI 子系统）。

### 不用预设

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
.\build\Release\webview-demo.exe
```

可选参数：

| 参数 | 作用 |
|---|---|
| `--port 8080` | 用固定端口（默认自动挑一个空闲端口） |
| 构建时 `-DWEBVIEW_DEMO_CONSOLE=ON` | 仅 Windows：编译成控制台程序，能直接看到日志（调试用） |

```powershell
# 控制台版（能看到 [http] 访问日志），等价于上面的 x64-console 预设
cmake --preset x64-console
cmake --build --preset console-release
.\build-console\Release\webview-demo.exe --port 8080
```

## 界面里能验证什么

| 区域 | 演示内容 | 涉及技术 |
|---|---|---|
| ① HTTP API | `fetch` 调用 `/api/hello`、`/api/time`、`/api/info`、`/api/assets`、`/api/echo`、`POST /api/add` | cpp-httplib |
| ② 原生调用 | `cppNativeEcho()`、`cppNativeHandle()`、`cppCloseWindow()`，**不经过 HTTP** | webview `webview::bind` |
| ③ 通信日志 | 每次请求/响应的原始 JSON | 前端 JS |

`GET /api/assets` 会列出被内嵌进可执行文件的资源（路径 / MIME / 字节数），可以直观看到
"前端资源就在二进制里"这件事；把 www 目录删掉，程序照样能跑。

## 开发辅助

```bash
# 任意 clang-format 19.x 均可
#   Windows: "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-format.exe"
#   Linux:   sudo apt install clang-format   (或复用 VS 之外任意 19.x)
clang-format -i src/main.cpp www/app.js
clang-format --dry-run --Werror src/main.cpp www/app.js   # 0 违规，可用作格式门禁

node --check www/app.js                                   # 前端语法检查
```

> `.clang-format` 里的 CSS 段对 `www/style.css` **不生效**——clang-format 不支持 CSS，
> 拿它去校验 style.css 会按 C++ 解析、报一堆假违规。

| 文件 | 作用 |
|---|---|
| `.clang-format` | C++20 与 JS 的格式化规则（当前代码实测 0 违规；`SortIncludes: false` 以保护 `WebAssets.h` 的包含顺序） |
| `.editorconfig` | UTF-8 + LF；C++ 4 空格、前端 2 空格、Markdown 保留行尾空格；IDE 自动生效 |
| `.gitattributes` | 仓库内统一 LF 存储，Windows 脚本保留 CRLF，二进制文件标记 |
| `.gitignore` | `build*/`、`.vs/`、MSVC 中间产物、WebView2 运行时数据目录 |
| `CMakePresets.json` | 四个 Windows 预设 + `linux` / `linux-debug`；用 `condition` 按平台过滤，`cmake --list-presets` 不会列出不适用的 |
| `.github/workflows/build.yml` | push / PR 时 `windows-latest`（当前镜像只有 VS2026）配置 + 编译 + 上传 exe；`ubuntu-22.04` 配置 + 编译 + **Xvfb 无头冒烟测试** + 上传 ELF；也供 release.yml 复用（`workflow_call`） |
| `.github/workflows/release.yml` | 推 `v*` tag 时复用 build.yml 编两个平台，再用 CI 自带的 `GITHUB_TOKEN` 建 Release 并把两个二进制挂上去 |
| `release-notes/vX.Y.Z.md` | 该版本的发布说明，publish job 会优先读它当 Release 正文 |
| `THIRD_PARTY_NOTICES.md` | 三方组件与许可声明（再分发前请留意） |

## 各库在代码里的落点

```cmake
cpp_embedlib_add(WebAssets FOLDER ${CMAKE_CURRENT_SOURCE_DIR}/www NAMESPACE Web)  # 生成 Web::FS
target_link_libraries(webview-demo PRIVATE
    WebAssets                     # 内嵌资源（同时把 C++20 要求传播过来）
    cpp-embedlib-httplib          # 提供 httplib::mount(svr, Web::FS)
    httplib::httplib              # HTTP 服务器
    nlohmann_json::nlohmann_json  # JSON 序列化 / 解析
    webview::core)                # webview（header-only；C API 与 C++ API 同一个头文件）
```

```cpp
#include "WebAssets.h"             // 由 cpp-embedlib 生成
#include <cpp-embedlib-httplib.h>  // httplib::mount
#include <httplib.h>
#include <nlohmann/json.hpp>       // JSON 序列化 / 解析（header-only）
#include <webview/webview.h>       // webview 0.12 的 C++ API（header-only）

httplib::mount(svr, Web::FS);                        // 内嵌资源挂到 "/"
int port = svr.bind_to_any_port("127.0.0.1");        // 随机空闲端口
std::thread t([&] { svr.listen_after_bind(); });     // 后台线程跑服务

webview::webview w(true /* debug */, nullptr);       // 必须主线程；析构即销毁窗口
w.set_title("...");
w.set_size(1080, 780, WEBVIEW_HINT_NONE);
w.bind("cppNativeEcho", &on_native_echo, nullptr);   // JS → C++（不走 HTTP）
w.navigate(url);                                     // 打开本地地址
w.run();                                             // 阻塞，直到窗口关闭
svr.stop(); t.join();
```

### JSON 处理：手写转义 → nlohmann/json

改之前 `src/main.cpp` 里有 53 行自己写的 JSON 工具（`json_escape` 转义、`first_json_string`
解析、`format_number` 数字格式化）外加 8 处字符串拼接。现在这些全部交给 nlohmann/json：

```cpp
// 之前：拼字符串 + 自己转义，字段顺序要靠人肉维护
reply_json(res, "{\"you_said\":\"" + json_escape(q) +
                    "\",\"length\":" + std::to_string(q.size()) + "}");

// 现在：对象一建就完事，转义和格式都是库的事
reply_json(res, json{{"you_said", q}, {"length", q.size()}});
```

`src/main.cpp` 里只留了三个小工具，其余全部删掉：

| 工具 | 作用 |
|---|---|
| `using json = nlohmann::ordered_json;` | 别名。**用 `ordered_json` 而不是 `nlohmann::json`**：后者的底层是 `std::map`，键会按字母序输出，响应体读起来和以前完全不一样 |
| `json_text(value)` | 唯一出口，负责 `dump()`。里面固定带 `error_handler_t::replace`（见坑 11） |
| `first_json_string(args)` | 把 webview 传来的 `["hello"]` 取出第一个字符串，换成 `json::parse(args, nullptr, false)` |

**代价**：CI 上 Linux 的编译步骤从 17 秒变成 21 秒、Windows 没变，
configure 一步没变（300 KB 的 `include.zip` 下载可以忽略）；
本机 VS2022 单 TU 实测 +2.9 秒；静态体积 +约 100 KB。这个项目只有一个 `.cpp`，
所以「nlohmann 编译慢」这件事在这里总共就值几秒。

**行为上唯二的区别**（下一次发版会体现）：

1. `/api/add` 返回 `{"a":12.0,"b":30.0,"sum":42.0}` 而不是 `{"a":12,"b":30,"sum":42}`
   —— 这三个值本来就是 `double`，JSON 库只是没替我们"猜"成整数。想输出整数得自己判一下再塞
   `long long`，这里故意不判，保持代码简单。
2. `/api/info` 多了一个 `"nlohmann/json":"3.12.0"` 字段。

### C API ↔ C++ API 对照

两套接口在同一个头文件里，本项目用的是右边那列：

| 功能 | C API | C++ API |
|---|---|---|
| 创建 / 销毁 | `webview_create(1, nullptr)` / `webview_destroy(w)` | `webview::webview w(true, nullptr);`（RAII，析构自动销毁） |
| 标题 / 尺寸 | `webview_set_title` / `webview_set_size` | `w.set_title(...)` / `w.set_size(1080, 780, WEBVIEW_HINT_NONE)` |
| 打开页面 | `webview_navigate` | `w.navigate(url)` |
| 直接塞 HTML | `webview_set_html` | `w.set_html(html)` |
| JS → C++ | `webview_bind(w, "name", fn, arg)` | `w.bind("name", fn, nullptr)` |
| 回传结果 | `webview_return(w, id, 0, json)` | `w.resolve(id, 0, json)` |
| 注入脚本 | `webview_init(w, js)` | `w.init(js)` |
| 执行脚本 | `webview_eval(w, js)` | `w.eval(js)` |
| 投递到 UI 线程 | `webview_dispatch(w, fn, arg)` | `w.dispatch(fn)` |
| 取原生句柄 | `webview_get_window(w)` | `w.window()`（返回 `result<void*>`，要判 `.ok()`） |
| 主循环 / 结束 | `webview_run(w)` / `webview_terminate(w)` | `w.run()` / `w.terminate()` |
| 错误处理 | 返回 `webview_error_t`，`webview_create` 失败返回 `nullptr` | 抛 `webview::exception`；`noresult` / `result<T>` 可用 `.ok()` / `.ensure_ok()` |

C++ 侧的三个额外注意点（都在 `src/main.cpp` 里体现了）：

- **构造函数没有默认参数**：必须写全 `webview::webview w(true, nullptr)`，不能只写 `webview::webview w;`。
  构造时若 WebView2 不可用会抛 `webview::exception`，所以整段要包 `try / catch`。
- **回调签名是 `binding_t`**：`std::function<void(std::string id, std::string args, void* arg)>`，
  比 C 版的 `const char*` 更省事；`args` 是 **JSON 数组**字符串（形如 `["hello"]`），
  用 `resolve()` 回传，而回传的 `result` 也**必须是合法 JSON 文本**——所以这边统一走
  `json_text(json{...})`，不要直接扔一个裸字符串进去。
- **没有公开的版本查询函数**：C 的 `webview_version()` 其实就是返回 `webview::detail::library_version_info`，
  C++ 这边没有等价公开接口，所以 `/api/info` 直接用头文件里的公开宏 `WEBVIEW_VERSION_NUMBER`。

## 需要注意的几个坑（都实测踩过）

1. **webview 0.12 里 C API 和 C++ API 是并存的，别信“C++ API 已被移除”的说法。**
   这条我最初搞错了，纠正如下：`core/include/webview/webview.h` 里**搜不到 `class webview`**，
   但那不代表没有 C++ API——公开类型 `webview::webview` 是个 **type alias**：

   ```cpp
   // webview.h:4367 / 4374
   using browser_engine = detail::win32_edge_engine;   // 各平台各挑一个（GTK / Cocoa / Edge）
   namespace webview { using webview = browser_engine; }
   ```

   真正的实现在 `webview::detail::engine_base`（webview.h:1221，暴露
   `set_title` / `set_size` / `navigate` / `set_html` / `init` / `eval` / `bind` / `run` …）
   和三个平台子类 `gtk_webkit_engine` / `cocoa_wkwebview_engine` / `win32_edge_engine`。
   官方 README 的第一个示例就是 **C++ Example**（`webview::webview w(false, nullptr);`），
   `examples/basic.cc` 同理，实测能编过。

   更关键的是**依赖方向**：C API 是包在这套 C++ 类外面的薄壳——
   `webview_create()` 就是 `new webview::webview{...}`，其余 C 函数清一色转发到成员函数（webview.h:4389 起）。

   两种写法都能用、都编得过；本项目原来用 C API，现已按对照表整体换成 C++ API。
   顺带一个教训：**判断某个 API 是否还在，不能只搜 `class X`**——别名（`using`）和宏同样可能是入口。

2. **webview 必须在 UI 线程调用（C API / C++ API 都一样）。**
   `eval` / `terminate` 这类调用不会自动切线程——在子线程里调用会“返回成功但毫无效果”
   （底层 `ICoreWebView2::ExecuteScript` 只能在 UI 线程跑）。
   子线程要操作窗口，用 `w.dispatch(fn)`（C 版是 `webview_dispatch`）投递到主线程；
   `w.bind()` 注册的回调本身就是在主线程执行的，可以直接调用这些成员函数。

3. **`w.init(js)` 注入的脚本只对“之后创建”的文档生效。**
   想让它在首页就生效，必须在 `w.navigate()` **之前**调用，否则第一次加载的页面不会执行它。

4. **首次配置要联网。** 四个库走 `FetchContent`；Windows 上 webview 还会自动从 nuget.org
   拉 `Microsoft.Web.WebView2` SDK（默认 1.0.1150.38）。如果 nuget 不可达：
   手动下载 `Microsoft.Web.WebView2` 的 nupkg 并解压，然后配置时加
   `-DMSWebView2_ROOT=<解压目录>`（目录里要有 `build/native/include/WebView2.h`）。
   默认启用 webview 内置的 WebView2Loader 实现，所以**不需要**往输出目录拷 `WebView2Loader.dll`。

5. **`WebAssets.h` 是构建时生成的**，第一次编译前 IDE 会在 `#include "WebAssets.h"` 上标红，
   `cmake --build` 一次之后就正常了。

6. **（仅 Windows）无控制台窗口的 GUI 程序看不到 `printf`。**
   项目默认 `WIN32_EXECUTABLE`（不弹黑框），日志走 `OutputDebugStringW`（VS 输出窗口 / DebugView 可见）。
   调试阶段用 `-DWEBVIEW_DEMO_CONSOLE=ON` 更方便。
   注意 GUI 子系统下 MSVC 默认找 `WinMain`，本项目用 `target_link_options(... "/ENTRY:mainCRTStartup")`
   保留标准 `main()` 入口。Linux 上这个开关没有意义——默认构建就能在终端里看到日志。
   （用 `*W` 而不是 `*A` 的原因见坑 12；带控制台构建时，中文日志还取决于
   控制台码页 `chcp 65001`，重定向到文件则不受影响。）

7. **端口冲突**：默认用 `bind_to_any_port` 自动挑空闲端口，窗口打开的就是该端口，不需要硬编码。

8. **Linux 上的三个硬门槛**（都是实测踩出来的，缺一个都编不过）：
   - **CMake ≥ 3.20**：Ubuntu 20.04 自带的 3.16 会直接拒绝配置（22.04 的 3.22 正好够）；
   - **GCC ≥ 10**（或等价的 clang）：GCC 9 的 libstdc++ 没有 `<span>`，cpp-embedlib 会报
     `fatal error: span: No such file or directory`；
   - **`pkg-config` + GTK3/WebKitGTK 开发包**：webview 在 **configure 阶段**就
     `find_package(PkgConfig REQUIRED)`，缺了会立刻 FATAL_ERROR（不会拖到编译才报错）。
     无头机器想真的跑起来还需要 `xvfb`。

9. **原生句柄不是同一个东西。** `w.window()` 在 Windows 上返回 `HWND`，在 Linux 上返回
   `GtkWidget *`——本项目把它当不透明指针打印，名字由 `k_native_handle_name` 按平台切换。
   另外 `title` / `set_size` 的单位、窗口管理器行为在各平台也不完全一致。

10. **别在预设里写死 Visual Studio 版本**（这条是 CI 跑红了才发现的）。
    `x64` 预设原来写的 `"generator": "Visual Studio 17 2022"`，而 GitHub 的
    `windows-latest`（现在是 Windows Server 2025）**已经只剩 VS2026**，
    配置直接报 `Generator Visual Studio 17 2022 could not find any instance of Visual Studio.`
    ——只装了 VS2026 的用户会撞到一模一样的错。现在改成**不指定 generator**，
    跟随本机默认（最新）的 Visual Studio：本机只有 VS2022 时仍然落到 `Visual Studio 17 2022`，
    行为不变；装了更新的 VS 也不会再报错。代价是一台机器上有多个 VS 时用的是最新的那个，
    要指定就手写 `cmake -S . -B build -G "Visual Studio 17 2022" -A x64`。

11. **nlohmann/json 有三个不看文档就会踩的地方**（这条是引入它的时候实测出来的）：
    - **默认的 `nlohmann::json` 会把键按字母序输出**（底层是 `std::map`），响应体一下子
      全变了样。要保留插入顺序必须用 **`nlohmann::ordered_json`**。
    - **`dump()` 默认会对非法 UTF-8 抛 `type_error.316`**。一个
      `GET /api/echo?q=%FF%FE` 就能把处理器抛进 500。要传
      `dump(-1, ' ', false, error_handler_t::replace)`，把坏字节换成 U+FFFD——
      输出反而是**合法** JSON（手写 `json_escape` 那时候是直接吐非法字节）。
    - **`double` 一律输出成 `42.0`**：`j["sum"] = 42.0` 不会变成 `42`。这不是 bug，
      是「库不替你猜」。想要整数就自己判一下塞 `long long`。
    另外 `FetchContent` 用的是 `include.zip` 而不是官方推荐的 `json.tar.xz`：
    后者带一份它自己的 `CMakeLists.txt`，而 `include.zip` 里只有头文件，我们包一个
    INTERFACE 目标就好，不用把第三方的构建脚本拉进来；顺手配了 `URL_HASH`，
    上游哪天换了 asset 会当场报错而不是悄悄编过。

12. **开了 `/utf-8` 之后，Win32 的 `*A` 接口全是坏的，`*W` 接口则必须自己转码**
    （这条是把错误对话框里的字读出来才发现的）。
    项目给 MSVC 加了 `/utf-8`（见 `CMakeLists.txt`，注释里写着"保证中文字符串字面量正确"），
    好处是源码和窄字符串统一 UTF-8、和 GCC 行为一致；代价是**任何把窄字符串递给 Win32 的地方
    都得先转成 UTF-16**：

    - `MessageBoxA` / `OutputDebugStringA` 会把 UTF-8 当成 ANSI 码页（本机 936）解释，
      中文照样糊——`*A` 并不能当退路。
    - 更隐蔽的是这种写法：`MessageBoxW(nullptr, std::wstring(msg.begin(), msg.end()).c_str(), ...)`。
      它是**按字节**往 `wchar_t` 里塞，一个汉字（UTF-8 3 字节）变成 3 个乱码字符；
      又因为 MSVC 的 `char` 是带符号的，字节 `0xE7` 会符号扩展成 `0xFFE7`，
      于是"端口 18087 已被占用"在对话框里显示成
      `￧ﾫﾯ￩ﾏﾣ 18087 ￩ﾷﾲ￨ﾢﾫ￩ﾍﾠ￧ﾔﾨ`——数字还在，中文全没了。
      `/W3` 下**编译器不会给任何警告**，GUI 子系统又没有控制台，所以只有把对话框里的字读出来
      才能发现（`FindWindow("#32770", ...)` + `GetWindowTextW` 就可以了）。

    修法是 `to_wstring_utf8()`（`MultiByteToWideChar(CP_UTF8, ...)`，见 `src/main.cpp`），
    并且 `flags` 传 0 而不是 `MB_ERR_INVALID_CHARS`：非法字节变成 U+FFFD 而不是直接失败，
    和 `json_text()` 用 `error_handler_t::replace` 是同一个态度——**报错信息本身不能因为
    混进一个坏字节就报不出来**。这条路径平时没人跑，但它正好是第一次用的人最容易撞上的那条
    （WebView2 运行时没装 → `fatal("webview 初始化/运行失败: ... 请确认已安装 WebView2 运行时")`），
    糊在那个对话框里等于什么都没说。

## 接口一览

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/`、`/style.css`、`/app.js` | 内嵌前端资源（html/css/js 的 MIME 自动识别） |
| GET | `/api/hello` | 返回问候语 + 服务器时间 |
| GET | `/api/time` | 本地时间 + 进程运行毫秒数 |
| GET | `/api/info` | 各库版本（cpp-httplib / webview / cpp-embedlib / nlohmann-json）、操作系统、PID、运行时长 |
| GET | `/api/assets` | 列出被 cpp-embedlib 内嵌的文件 |
| GET | `/api/echo?q=...` | 回显参数 |
| POST | `/api/add` | 表单 `a=..&b=..`，返回和（`double`，所以是 `42.0` 不是 `42`） |

JS → C++ 绑定：`cppNativeEcho(text)`、`cppNativeHandle()`、`cppCloseWindow()`。

## 如何发版

发布是完全「推 tag 触发」的：不需要手工编译，也不需要往网页上拖任何文件。

### 1. 确认要发的提交是绿的

`main` 上最后一次 CI 必须是绿的（看顶上徽章）。发布用的二进制是 CI 从这个 tag
**现场编出来的**，所以 main 绿 = 发出去的东西就是验证过的那份。

### 2. 定版本号

遵循 SemVer，**tag 名就是版本号**，`v` 前缀不能少（工作流按 `v*` 匹配）：

| 场景 | 例子 |
| --- | --- |
| 修 bug / 只改文档 | `v0.1.2`（patch） |
| 加功能 | `v0.2.0`（minor） |
| 破坏性改动 | `v1.0.0`（major） |
| 想先试发一版 | `v0.2.0-rc1` → 自动标成 **Pre-release**，不会顶掉 Latest |

### 3. 写发布说明（推荐，但不强制）

在 `release-notes/vX.Y.Z.md` 里写这个版本要说什么，纯 Markdown，会**原样**成为 Release
正文。没有这个文件时会退回读根目录 `RELEASE_NOTES.md`，再没有才用 GitHub 自动生成的
——而自动生成列的是「合并的 PR」，本仓库都是直接 push，所以它只会给一行 changelog
链接（v0.1.0 当时就是这样），因此建议手写。

可以照抄 `release-notes/v0.1.1.md` 的结构：这次改了什么 / 下载表 / compare 链接。

### 4. 提交 → 推 main → 打 tag

```bash
git add release-notes/v0.1.2.md          # 说明文件要先提交：publish 读的是 tag 里的内容
git commit -m "docs: 补上 v0.1.2 的发布说明"
git push origin main

git tag -a v0.1.2 -m "v0.1.2 - 一句话概括这次发了什么"
git push origin v0.1.2
```

`-a` 建议加（注解 tag），`-m` 里那句话就是 tag 自带的一句话摘要。

### 5. 看 Actions → Releases

推 tag 会触发 `release` 工作流：先并行编译两个平台（复用 `build.yml`），再 publish。
大约 6~8 分钟后，[Releases](https://github.com/leiddev/webview-httplib-demo/releases)
上应该出现：标题 = tag 名、正文 = 第 3 步写的说明、附件 =
`webview-demo-<tag>-windows-x64.exe` 和 `webview-demo-<tag>-linux-x64`。

### 出问题了怎么办

- **publish 失败**：不会留下半成品（`gh release create` 要么建好要么不建）。看失败那步的
  annotation —— 工作流会把 `gh` 的输出逐行贴上来 —— 修完之后删 tag 重推即可：

  ```bash
  git tag -d v0.1.2
  git push origin --delete v0.1.2
  # 改完代码/说明，重新 tag + push
  ```

- **Release 已经建好、只想改正文或换附件**：网页上 Releases → 该版本 → ✏️ Edit release，
  改完 Update 就行，**不用**重新发版。
- **想彻底重发**：先在网页删掉那个 Release，再按上面的步骤删 tag 重推。反过来（先删 tag）
  会让 Release 变成没有 tag 的孤儿，清理起来更麻烦。
- **已经发出去的 tag 不要改指向**（别 `push -f` 移动 tag）：老下载链接会指向新的提交内容，
  和已发出去的二进制对不上。

### 两个容易踩的坑

1. **别给 `build.yml` 的 push 触发再加 `tags`**：现在 tag 只由 `release.yml` 触发，它再
   `workflow_call` 复用 build.yml。两边都写，同一次 tag 会跑两遍全量构建。
2. **tag 名会被拼进附件名**：附件是 `webview-demo-${tag}-...`。另外必须能以 `v*` 匹配上，
   用 `release-0.1.2` 这种前缀的话工作流根本不会触发。

## 已验证的环境

**Windows**：Windows 10 22H2 (19045) x64 · VS2022 Community 17.14 · MSVC 14.44 · CMake 4.3.0 / VS 自带 3.31.6 ·
WebView2 Runtime 153.0.4234.48

**Linux**：Ubuntu 22.04.1 LTS（kernel 6.8，无显示器）· GCC 11.4.0 · CMake 3.22.1 · Ninja ·
WebKitGTK 2.50.4（`webkit2gtk-4.1`）+ GTK 3.24.33 + libsoup3 · Xvfb

依赖版本：cpp-httplib v0.38.0 · webview 0.12.0 · cpp-embedlib main · nlohmann/json 3.12.0

### Windows 实测记录

`build\Release\webview-demo.exe`，编译 0 警告，**562,176 字节**（CI 的 `windows-latest` 是 VS2026，
编出来 568,320 字节）：

- 窗口类名 `webview`、客户区 1080×780、标题与 `set_title()` 一致 → 构造 / `set_size` / `set_title` 生效；
- 服务端日志里先出现 `GET /`、`GET /style.css`、`GET /app.js`（是**窗口自己**来拉的）→ `navigate` 生效、页面渲染成功；
- 全部 7 个接口 200，`POST /api/add` 返回 `{"a":3.0,"b":4.0,"sum":7.0}`（`double` 的原样输出，见「JSON 处理」）；
  九个用例（六个接口 + 小数 + 缺参数 400 + 坏 UTF-8）的返回**全部能被 JSON 解析器吃掉**；
- 注入脚本串起三个绑定：`cppNativeEcho` 返回 `{"source":"webview::bind → C++",…}`、
  `cppNativeHandle` 返回 `HWND = 0x…`、`cppCloseWindow` 之后进程自行退出（ExitCode=0）→
  `bind` / `resolve` / `window` / `terminate` 与 RAII 析构都正常；
- **错误对话框里的字是读出来核对过的**（这条只能这么做，见坑 12）：先占住端口再启动，
  然后用 `FindWindow("#32770", "webview-httplib-demo")` + `GetWindowTextW` 把对话框正文取回来，
  逐字符打印码点——修之前是 `￧ﾫﾯ￩ﾏﾣ 18087 ￩ﾷﾲ￨ﾢﾫ￩ﾍﾠ￧ﾔﾨ`
  （`U+FFE7 U+FFAB U+FFAF …`，UTF-8 字节被符号扩展的结果），
  修之后是 `端口 18087 已被占用`（`U+7AEF U+53E3 …`）。

### Linux 实测记录

在 Ubuntu 22.04（无显示器）上把 `git archive HEAD` 的干净快照跑了一遍：

- **configure 146 秒** —— 首次要把四个依赖全从网上下下来，基本全是网络时间；依赖已经在本地时
  再 configure 是 33 秒。webview 自动选中 `webkit2gtk-4.1` 2.50.4 + `gtk+-3.0` 3.24.33 + libsoup3；
- **build 22 秒、0 警告**，产出 **944,760 字节** ELF，链接 `libwebkit2gtk-4.1` / `libjavascriptcoregtk-4.1`；
  cpp-embedlib 在 Linux 上正常生成 `_data_index_html` / `_data_app_js` / `_data_style_css` → `libWebAssets.a`；
- 用 Xvfb 无头运行（`git archive` 出来那份代码**一行没改**）：7 个接口全 200，
  `POST /api/add` 返回 `{"a":12.0,"b":30.0,"sum":42.0}`，返回**全部是合法 JSON**，
  缺参数返回 400，带坏 UTF-8（`?q=%FF%FE`）既不 500 也不吐非法 JSON；
  日志最前面是**窗口自己**发起的 `/`、`/style.css`、`/app.js`、`/api/info` → GTK + WebKitGTK 渲染链路通；
- **JS→C++ 桥**在 GTK 后端同样正常。这条 HTTP 断言测不到，所以另外在一个临时副本里塞了段开机自检
  （只改那份副本的 `app.js`，仓库代码没动）：JS 先 `await cppNativeEcho('自检 he said "hi"\tEND')`，
  再把**拿到的东西 `JSON.stringify` 之后回传一次**。日志里出现了回传的那次请求，就说明返回值确实
  被 JS 侧的 `JSON.parse` 吃下去了；`cppNativeHandle` 返回 `GtkWidget * = 0x…`，
  `cppCloseWindow` 之后进程自行退出（exit code 0）；
- `localtime_r` 分支正确，返回真实本地时间。

> 两个平台上 `cppNativeEcho` 的 `chars` 都是 **23**、吐出来的 JSON 结构逐字节相同
> （Windows 那边 `cppNativeHandle` 是 `HWND = 0x…`、Linux 是 `GtkWidget * = 0x…`）——
> 说明转义、UTF-8、数字格式这些都不依赖平台。

> 同一份代码在 Windows 和 Linux 上的 `/api/info` 分别返回 `"os":"windows"` / `"os":"linux"`
> 和各自真实的 PID —— 这两处正是这次移植时修掉的平台相关 bug。

### GitHub Actions 实测记录

第一次把代码推上去时，Linux job 一次通过、Windows job 挂在 configure。
摸到的 runner 实情（`windows-latest` 现在长这样了）：

| 镜像 | 系统 | CMake | Visual Studio |
|---|---|---|---|
| `windows-latest` | Windows Server 2025 (26100) | 4.4.3 | **Enterprise 2026**（没有 VS2022） |
| `windows-2022` | Windows Server 2022 (20348) | 3.31.6 | Enterprise 2022 |
| `ubuntu-22.04` | Ubuntu 22.04 | — | — |

- **`ubuntu-22.04` job 全绿**：配置 + 编译 + `ldd` 校验 WebKitGTK + Xvfb 无头冒烟
  （7 个接口、内嵌资源、日志里确认是窗口自己拉的页面）；
- **Windows job 的报错**是 `Generator Visual Studio 17 2022 could not find any instance of Visual Studio.`
  → 见坑 10，预设里不再写死 VS 版本后即通过。
