# Commander capability implementation status (2026-10-23)

This is an incremental migration, **not full Remote Desktop Commander feature parity**. Existing editor, LSP, Git, SSH, ngrok, and tray behavior is retained.

## Implemented / Windows-tested

| ID | Scope delivered | Remaining work |
|---|---|---|
| F01 | list_directory (bounded recursion), get_file_info, read_multiple_files (UTF-8), create_directory, move_file | Streaming large files; binary-safe editing; remote equivalents |
| F02 | edit_block exact matching, SHA-256 expectedSha256 precondition, dryRun preview, atomic QSaveFile write, dirty-editor guard | Line-range editing, graphical diff preview and stronger cross-process atomic compare-and-swap |
| F03 | search_workspace by filename or text, regex, glob, result limits | Background/streaming sessions, search progress, pagination |
| F04 | Persistent terminal, start_process, read_process_output with byte cursor, interact_with_process, list_sessions, force_terminate | Prompt detection and portable pseudo-terminal/ConPTY support |
| F05 | list_processes; kill_process requires confirm=true and rejects self/low PID | Permission UI, robust CPU usage on Windows, process tree details |
| F06 | read_document extracts DOCX text and first XLSX sheet; PDF text with external pdftotext | DOCX/XLSX/PDF editing and generation, images/tables/formatting, multiple worksheets |
| F07 | Persistent, named SSH target profiles (register/list/select/remove), local fallback | Device pairing, online telemetry, authenticated cloud relay, UI integration |
| F08 | Loopback-only MCP, CORS deny-by-default for foreign browser origins with explicit allowlist (requires Bearer), required Bearer token before ngrok, canonical path validation, bounded I/O, process-kill confirmation | Per-tool authorization, consent UX, command sandbox or allowlist; shell tools still execute arbitrary commands |
| F09 | In-memory history (last 200 calls; tool name, success, duration, time only) and uptime/counter metrics | Opt-in persistent history, UI and searchable diagnostics |
| F10 | Headless MCP mode, 37 Windows PowerShell end-to-end assertions and PR-triggered Windows/Qt smoke workflow | Linux/macOS CI, fuzzing and protocol conformance tests |
| F11 | SSH tool support retained; additional remote symlink-escape verification via realpath -m | Pooled SSH sessions, remote LSP, atomic remote writes, actual Linux SSH integration tests |
| F12 | CMake minimum 3.24; build_unix.sh; headless QCoreApplication without display plugin; platform-sensitive path handling | Native Linux/macOS builds, OS-native keychains, installers, ngrok auto-installers |

## Windows quick verification

On a system with Visual Studio 2026, Qt 6.8.3 MSVC x64 and CMake:

    cmake -S desktop -B desktop/build-check-vs2026 -G "Visual Studio 18 2026" -A x64 -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
    cmake --build desktop/build-check-vs2026 --config Release --parallel 4
    powershell -NoProfile -ExecutionPolicy Bypass -File desktop/tests/smoke.ps1

The smoke test starts the compiled executable with --headless and a temporary workspace. No Office installation is needed to test the included minimal OOXML fixtures.

## Linux/macOS build attempt

    QT_ROOT=/path/to/qt6 ./desktop/build_unix.sh

Or use CMake directly with an installed Qt6 package. This is only a build entrypoint: it has not been run on either OS. On Linux/macOS the current credential implementation does **not** persist secrets. The desktop GUI and ngrok packaging have not been validated.

## Headless MCP

    VSCODE_MCP_AUTH_TOKEN="strong-secret" ./vscode-mcp --headless --workspace /path/to/project --port 3333

On Windows set the environment variable in PowerShell before starting. HTTP stays bound to 127.0.0.1. The headless authentication token comes from the environment variable; avoid publishing a public tunnel without authentication.

Cross-origin browser requests are denied by default. Loopback origins matching the actual MCP port are allowed; foreign origins require **both** a Bearer token and an explicit allowlist entry. The Qt GUI exposes the comma-separated **允许的浏览器来源** field; in headless mode you can set `VSCODE_MCP_ALLOWED_ORIGINS=https://trusted.example`. A browser still needs to send `Authorization: Bearer ...` when enabled. Do not add untrusted websites to this list.

## Security model (important)

The workspace guard constrains the dedicated filesystem tools. Terminal commands are **not sandboxed**: a trusted agent with access to run_terminal_command or spawn_terminal can execute arbitrary commands as the logged-in OS user and can access files outside the workspace. Do not expose MCP credentials publicly. For untrusted agents, run in an isolated OS account or container and restrict network access.

SSH profile storage contains SSH config aliases and remote workspace roots, **not private keys**. Selecting a profile changes the MCP backend target for this process; the desktop editor has a separate UI remote setting. The remote canonical path check requires GNU realpath on the SSH server.

The first-sheet Excel parser, Word text extraction and PDF CLI text extraction are deliberately limited. They do not preserve rich document formatting or provide editing semantics.

## Recommended follow-on stages

1. **Security and reliability**: explicit per-tool permissions, per-device approval, structured protocol errors, cancellation, HTTP stress/fuzz coverage, and streaming search.
2. **Rich files and processes**: diff-preview editing with revision checks, ConPTY-based terminal handling, reliable process metrics, cross-platform document engines and writers.
3. **Remote devices**: authenticated pairing, encrypted transport, connection recovery, remote LSP, file transfer and UI management.
4. **Platform delivery**: Linux/macOS native builds and installers, secure local credential stores, signed release artifacts, CI on all three OSes.

Each stage should ship with tests before merging into main.
