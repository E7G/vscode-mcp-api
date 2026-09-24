# 独立版 MCP 桌面应用（Qt Widgets）

Windows x64 原生 C++/Qt 6 应用。**运行不需要 VS Code、Node.js 或原版扩展**。内置标签编辑器、托盘、深浅色主题、MCP HTTP、终端管理、Git 工具和 ngrok 隧道。保留原版 27 个 MCP 工具名，额外提供 `git_status`、`git_diff`。

## 使用

1. 解压整个发行包，运行 `vscode-mcp.exe`，选工作区。也可用 `vscode-mcp.exe --workspace C:\project` 临时指定目录。
2. 将面板显示的 `http://127.0.0.1:<实际端口>/mcp` 配给 MCP 客户端。默认端口 3333；被占用时自动在后续 20 个端口中选择。
3. 默认可留空 Bearer 令牌，与原版一样无需验证；设置后请求须带 `Authorization: Bearer <token>`。令牌存入 Windows Credential Manager。
4. C/C++ 语言工具使用发行包自带的 [clangd](https://clangd.llvm.org/installation)。其他语言的 LSP 是按需启动的可选组件：Python `pylsp` 或 `pyright-langserver`、JS/TS `typescript-language-server`、Rust `rust-analyzer`、Go `gopls`、Java `jdtls`、C# `csharp-ls`。放进 `PATH` 即可。可以在 Qt `QSettings` 的 `languageServers/<language>` 中覆盖启动命令。
5. 要使用 ngrok，填入真实 authtoken 和可选的账号保留域名；首次会自动下载 ngrok Agent。保存了固定域名、ngrok authtoken 和 MCP Bearer 令牌时，应用启动后会自动建立隧道。公网地址取自 Agent Inspector；此功能需要 ngrok 账号和网络。免费 ngrok 域名的浏览器警告页可通过请求头 `ngrok-skip-browser-warning: 1` 跳过。

本机健康检查：`http://127.0.0.1:<实际端口>/health`。关闭窗口只隐藏到托盘；右键托盘图标选“退出”可彻底退出。

## 兼容范围

文件、编辑器快照/标签/选区、差异预览、终端和 Git 工具由 Qt 进程直接实现。定义、引用、悬停、符号、诊断、代码操作、重命名通过标准 LSP 实现。`search_workspace_symbols` 优先查已启动的语言服务，缺少服务时退化为文本搜索。`execute_vscode_command` 目前只映射保存、打开、关闭当前编辑器三个命令；任意 VS Code 扩展命令需要 VS Code 运行时，独立应用不提供。原版自动上下文推送和 Cloudflare/localtunnel 管理未移植。

## VS2026 构建

前提：Visual Studio 2026 C++ 工具链、CMake 4.2+、Qt 6 MSVC x64。

```powershell
$env:QT_ROOT = 'C:\Qt\6.8.3\msvc2022_64'
.\desktop\build_windows.ps1
```

脚本编译 Release，调用 `windeployqt`，复制 MSVC 运行库，首次从 [clangd 官方发行版](https://github.com/clangd/clangd/releases/tag/22.1.6)获取 Windows 二进制到 `desktop/.cache`，并放入 `desktop/dist/lsp/clangd`。交付时要打包整个 `desktop/dist`，不要仅复制 EXE。
