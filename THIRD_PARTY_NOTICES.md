# 第三方组件说明

本项目自身代码以 [MIT](./LICENSE) 许可发布。构建时（或运行时）会用到以下第三方组件，
它们各自遵循自己的许可协议，**再分发时请一并保留其许可声明**。

## 构建时由 CMake 自动获取

| 组件 | 版本 | 许可 | 用途 | 地址 |
|---|---|---|---|---|
| **tinyexpr-plusplus** | commit `404688c` | zlib | **计算引擎的核心**：表达式解析与求值 | https://github.com/Blake-Madden/tinyexpr-plusplus |
| cpp-httplib | v0.38.0 | MIT | 本地 HTTP 服务器 / 客户端 | https://github.com/yhirose/cpp-httplib |
| webview | 0.12.0 | MIT | 跨平台 WebView 封装（C API） | https://github.com/webview/webview |
| cpp-embedlib | main | MIT | 把前端资源编译进可执行文件 | https://github.com/yhirose/cpp-embedlib |
| nlohmann/json | v3.12.0 | MIT | JSON 的序列化 / 解析 | https://github.com/nlohmann/json |
| Microsoft.Web.WebView2 SDK | 1.0.1150.38 | Microsoft 软件许可条款 | 提供 `WebView2.h` 等头文件（仅 Windows 配置期从 nuget.org 下载） | https://www.nuget.org/packages/Microsoft.Web.WebView2 |

### tinyexpr-plusplus 的许可与来源链

`tinyexpr-plusplus` 的许可为 **zlib**（宽松许可，允许闭源再分发，但要求保留版权声明与许可原文，
且不得以此项目的名义做背书）：

```
tinyexpr++

Copyright (c) 2022 Blake Madden
Copyright (c) 2015-2020 Lewis Van Winkle

This software is provided 'as-is', without any express or implied warranty. In no
event will the authors be held liable for any damages arising from the use of this
software.

Permission is granted to anyone to use this software for any purpose, including
commercial applications, and to alter it and redistribute it freely, subject to the
following restrictions:

1. The origin of this software must not be misrepresented; you must not claim that
   you wrote the original software. If you use this software in a product, an
   acknowledgment in the product documentation would be appreciated but is not
   required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

版本继承链：原始项目是 [tinyexpr](https://github.com/codeplea/tinyexpr)（作者 Lewis Van Winkle，
同样为 zlib 许可），`tinyexpr++` 是它的 C++ 重写与扩展。两段版权声明在本项目中都保留了。

本仓库**没有修改** tinyexpr++ 的源码，只新增了一个把它编成静态库的 CMake target
（`crosscalc_tinyexpr`，见 `CMakeLists.txt`）。它按官方推荐的用法被使用，未打补丁；
校验和锁定的是上游仓库的具体 commit，见 `CMakeLists.txt` 里的 `tinyexpr_plusplus` FetchContent 声明。

<details>
<summary>本项目的计算层与它的分工（为什么不自己写计算函数）</summary>

所有数学求值都交给 `te_parser`，本项目只在外面做三件事：模式的输入校验、
错误信息的中文化、以及**显示层**的进制格式化。设计取舍见
[docs/design-decisions.md](docs/design-decisions.md)。

</details>


## 运行时依赖（不在本仓库内，也不需要随包分发）

| 组件 | 说明 |
|---|---|
| Microsoft Edge WebView2 Runtime | webview 在 Windows 上依赖它来渲染页面。Win10/11 通常随 Edge 预装；缺失时应用会启动失败，可从 https://developer.microsoft.com/microsoft-edge/webview2/ 安装 Evergreen Runtime。许可条款见微软官方页面。 |

## 说明

- 本项目默认启用 webview 的**内置 WebView2Loader 实现**，因此不需要在输出目录附带 `WebView2Loader.dll`。
- `crosscalc.exe` 内嵌了 `www/` 下的前端资源（HTML/CSS/JS），这些文件由本项目自行编写，随本项目许可发布。
- Linux 版链接系统的 WebKitGTK（`libwebkit2gtk-4.1` / `libgtk-3`），它们由发行版提供，不随本项目分发，
  许可为 LGPL-2.1+；再分发二进制时请遵循对应发行版与 LGPL 的要求。
