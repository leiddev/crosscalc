# 第三方组件说明

本项目自身代码以 [MIT](./LICENSE) 许可发布。构建时（或运行时）会用到以下第三方组件，
它们各自遵循自己的许可协议，**再分发时请一并保留其许可声明**。

## 构建时由 CMake 自动获取

| 组件 | 版本 | 许可 | 用途 | 地址 |
|---|---|---|---|---|
| cpp-httplib | v0.38.0 | MIT | 本地 HTTP 服务器 / 客户端 | https://github.com/yhirose/cpp-httplib |
| webview | 0.12.0 | MIT | 跨平台 WebView 封装（C API） | https://github.com/webview/webview |
| cpp-embedlib | main | MIT | 把前端资源编译进可执行文件 | https://github.com/yhirose/cpp-embedlib |
| nlohmann/json | v3.12.0 | MIT | JSON 的序列化 / 解析 | https://github.com/nlohmann/json |
| Microsoft.Web.WebView2 SDK | 1.0.1150.38 | Microsoft 软件许可条款 | 提供 `WebView2.h` 等头文件（仅 Windows 配置期从 nuget.org 下载） | https://www.nuget.org/packages/Microsoft.Web.WebView2 |

## 运行时依赖（不在本仓库内，也不需要随包分发）

| 组件 | 说明 |
|---|---|
| Microsoft Edge WebView2 Runtime | webview 在 Windows 上依赖它来渲染页面。Win10/11 通常随 Edge 预装；缺失时应用会启动失败，可从 https://developer.microsoft.com/microsoft-edge/webview2/ 安装 Evergreen Runtime。许可条款见微软官方页面。 |

## 说明

- 本项目默认启用 webview 的**内置 WebView2Loader 实现**，因此不需要在输出目录附带 `WebView2Loader.dll`。
- `webview-demo.exe` 内嵌了 `www/` 下的前端资源（HTML/CSS/JS），这些文件由本项目自行编写，随本项目许可发布。
