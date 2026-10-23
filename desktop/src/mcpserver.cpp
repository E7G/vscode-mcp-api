#include "mcpserver.h"
#include "editorbridge.h"
#include "languageservice.h"
#include "workspacetools.h"
#include "documenttools.h"
#include <QSettings>

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QJsonArray>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QUuid>
#include <QSaveFile>
#include <QTcpSocket>
#include <QTimer>
#include <QDesktopServices>
#include <QElapsedTimer>
#include <QDateTime>
#include <QUrl>
#include <QStandardPaths>
#include <QRegularExpression>
#include <exception>

namespace {
QJsonObject rpcError(const QJsonValue &id, int code, const QString &message)
{
    return {{"jsonrpc", "2.0"}, {"id", id},
            {"error", QJsonObject{{"code", code}, {"message", message}}}};
}
QString textValue(const QJsonValue &value) { return value.isString() ? value.toString() : QString{}; }
}

McpServer::McpServer(QObject *parent) : QObject(parent), tcpServer_(new QTcpServer(this))
{
    connect(tcpServer_, &QTcpServer::newConnection, this, &McpServer::acceptConnections);
    uptime_.start();
}

int McpServer::port() const { return port_; }
int McpServer::connectionCount() const { return connections_.load(); }
QString McpServer::workspace() const { return workspace_; }

void McpServer::start(int port)
{
    if (tcpServer_->isListening()) return;
    quint16 selectedPort = static_cast<quint16>(port);
    if (!tcpServer_->listen(QHostAddress::LocalHost, selectedPort)) {
        const QString firstError = tcpServer_->errorString();
        if (tcpServer_->serverError() != QAbstractSocket::AddressInUseError) {
            emit serverStateChanged(false, port, firstError);
            return;
        }
        bool bound = false;
        const int lastPort = qMin(65535, port + 20);
        for (int candidate = port + 1; candidate <= lastPort; ++candidate) {
            if (tcpServer_->listen(QHostAddress::LocalHost, static_cast<quint16>(candidate))) {
                selectedPort = static_cast<quint16>(candidate);
                bound = true;
                emit logMessage(QStringLiteral("端口 %1 被占用，已自动切换到 %2").arg(port).arg(candidate));
                break;
            }
            if (tcpServer_->serverError() != QAbstractSocket::AddressInUseError) break;
        }
        if (!bound) {
            emit serverStateChanged(false, port,
                QStringLiteral("端口 %1 无法绑定；自动尝试的相邻端口也不可用。最后错误：%2").arg(port).arg(tcpServer_->errorString()));
            return;
        }
    }
    port_ = selectedPort;
    emit logMessage(QStringLiteral("MCP HTTP 服务监听 127.0.0.1:%1").arg(port_));
    emit serverStateChanged(true, port_, {});
}

void McpServer::stop()
{
    if (tcpServer_->isListening()) tcpServer_->close();
    port_ = 0;
    emit serverStateChanged(false, 0, {});
}

void McpServer::setWorkspace(const QString &path)
{
    const QString next = QDir(path).canonicalPath();
    if (next == workspace_) return;
    qDeleteAll(languages_);
    languages_.clear();
    workspace_ = next;
}
void McpServer::setRemoteTarget(const QString &sshHost, const QString &remoteRoot)
{
    sshHost_ = sshHost.trimmed();
    remoteRoot_ = remoteRoot.trimmed();
}
void McpServer::setAuthToken(const QString &token) { authToken_ = token; }
void McpServer::setEditorBridge(EditorBridge *editor) { editor_ = editor; }

void McpServer::acceptConnections()
{
    while (tcpServer_->hasPendingConnections()) {
        QTcpSocket *socket = tcpServer_->nextPendingConnection();
        socket->setProperty("requestBuffer", QByteArray{});
        connect(socket, &QTcpSocket::readyRead, this, &McpServer::readRequest);
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QTcpSocket::disconnected, this, [this]() {
            connections_.fetch_sub(1); emit clientCountChanged(connectionCount());
        });
        connections_.fetch_add(1);
        emit clientCountChanged(connectionCount());
    }
}

void McpServer::readRequest()
{
    auto *socket = qobject_cast<QTcpSocket *>(sender());
    if (!socket) return;
    QByteArray buffer = socket->property("requestBuffer").toByteArray();
    buffer += socket->readAll();
    if (buffer.size() > 10 * 1024 * 1024) { sendHttp(socket, 413, "{\"error\":\"request too large\"}"); return; }
    const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        if (buffer.size() > 16 * 1024) { sendHttp(socket, 413, "{\"error\":\"headers too large\"}"); return; }
        socket->setProperty("requestBuffer", buffer); return;
    }
    constexpr qsizetype maxRequestBytes = 10 * 1024 * 1024;
    if (headerEnd > 16 * 1024) { sendHttp(socket, 413, "{\"error\":\"headers too large\"}"); return; }
    qsizetype bodyLength = 0;
    bool hasLength = false;
    const QByteArray header = buffer.left(headerEnd);
    for (const QByteArray &line : header.split('\n')) {
        const QByteArray low = line.trimmed().toLower();
        if (low.startsWith("transfer-encoding:")) {
            sendHttp(socket, 400, "{\"error\":\"transfer encoding not supported\"}"); return;
        }
        if (low.startsWith("content-length:")) {
            bool valid = false;
            const QByteArray value = low.mid(15).trimmed();
            const qlonglong parsed = value.toLongLong(&valid);
            if (!valid || parsed < 0 || (hasLength && parsed != bodyLength)) {
                sendHttp(socket, 400, "{\"error\":\"invalid content length\"}"); return;
            }
            bodyLength = parsed;
            hasLength = true;
        }
    }
    if (!hasLength && header.startsWith("POST ")) {
        sendHttp(socket, 411, "{\"error\":\"content length required\"}"); return;
    }
    if (bodyLength > maxRequestBytes - headerEnd - 4) {
        sendHttp(socket, 413, "{\"error\":\"request too large\"}"); return;
    }
    if (buffer.size() < headerEnd + 4 + bodyLength) {
        socket->setProperty("requestBuffer", buffer); return;
    }
    socket->setProperty("requestBuffer", QByteArray{});
    disconnect(socket, &QTcpSocket::readyRead, this, &McpServer::readRequest);
    handleRequest(socket, buffer.left(headerEnd + 4 + bodyLength));
}

void McpServer::handleRequest(QTcpSocket *socket, const QByteArray &request)
{
    const qsizetype headerEnd = request.indexOf("\r\n\r\n");
    const QByteArray header = request.left(headerEnd);
    const QByteArray first = header.left(header.indexOf('\n')).trimmed();
    const QList<QByteArray> parts = first.split(' ');
    const QByteArray method = parts.value(0);
    const QByteArray path = parts.value(1);
    QByteArray authorization;
    for (const QByteArray &line : header.split('\n')) {
        const QByteArray low = line.trimmed().toLower();
        if (low.startsWith("authorization:")) authorization = line.trimmed().mid(14).trimmed();
    }
    if (method == "OPTIONS") { sendHttp(socket, 204, ""); return; }
    if (path == "/health" && method == "GET") {
        const QJsonObject health{{"status", "ok"}, {"version", APP_VERSION},
                                 {"activeRequests", connectionCount()}, {"port", port_}};
        sendHttp(socket, 200, QJsonDocument(health).toJson(QJsonDocument::Compact)); return;
    }
    if (method != "POST" || path != "/mcp") { sendHttp(socket, 404, "{\"error\":\"not found\"}"); return; }
    if (!authToken_.isEmpty() && authorization != QByteArray("Bearer ") + authToken_.toUtf8()) {
        sendHttp(socket, 401, "{\"error\":\"unauthorized\"}"); return;
    }
    const QByteArray body = request.mid(headerEnd + 4);
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        sendHttp(socket, 400, QJsonDocument(rpcError(QJsonValue(), -32700, QStringLiteral("Parse error"))).toJson(QJsonDocument::Compact)); return;
    }
    const QJsonObject req = doc.object();
    const QJsonValue id = req.value(QStringLiteral("id"));
    if (id.isUndefined()) { sendHttp(socket, 202, ""); return; }
    const QJsonObject response = rpc(req);
    sendHttp(socket, 200, QJsonDocument(response).toJson(QJsonDocument::Compact));
    emit logMessage(QStringLiteral("MCP %1").arg(req.value(QStringLiteral("method")).toString()));
}

void McpServer::sendHttp(QTcpSocket *socket, int status, const QByteArray &body, const QByteArray &contentType)
{
    static const QHash<int, QByteArray> statusText{{200,"OK"},{202,"Accepted"},{204,"No Content"},
        {400,"Bad Request"},{401,"Unauthorized"},{404,"Not Found"},{405,"Method Not Allowed"},
        {411,"Length Required"},{413,"Payload Too Large"},{500,"Internal Server Error"}};
    QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + " " + statusText.value(status, "OK") + "\r\n";
    response += "Content-Type: " + contentType + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n";
    response += "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: Content-Type, Authorization, MCP-Protocol-Version\r\n";
    response += "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\nConnection: close\r\n\r\n" + body;
    socket->write(response); socket->disconnectFromHost();
    QTimer::singleShot(5000, socket, [socket]() { if (socket->state() != QAbstractSocket::UnconnectedState) socket->abort(); });
}

QJsonObject McpServer::rpc(const QJsonObject &request)
{
    const QString method = request.value(QStringLiteral("method")).toString();
    const QJsonValue id = request.value(QStringLiteral("id"));
    const QJsonObject params = request.value(QStringLiteral("params")).toObject();
    if (method == QStringLiteral("initialize")) {
        const QString version = params.value(QStringLiteral("protocolVersion")).toString(QStringLiteral("2025-03-26"));
        return {{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonObject{
            {"protocolVersion", version}, {"capabilities", QJsonObject{{"tools", QJsonObject{{"listChanged", false}}}}},
            {"serverInfo", QJsonObject{{"name", "vscode-mcp-qt"}, {"version", APP_VERSION}}},
            {"instructions", "独立桌面桥接服务：文件、工作区、Git 和终端工具运行在所选工作区中。"}}}};
    }
    if (method == QStringLiteral("ping")) return {{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonObject{}}};
    if (method == QStringLiteral("tools/list"))
        return {{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonObject{{"tools", toolList()}}}};
    if (method == QStringLiteral("tools/call")) {
        const QString name = params.value(QStringLiteral("name")).toString();
        bool isError = false;
        QElapsedTimer timer; timer.start();
        const auto value = callTool(name,
                                    params.value(QStringLiteral("arguments")).toObject(), &isError);
        if (isError) ++failedCalls_; else ++successfulCalls_;
        toolHistory_.append(QJsonObject{{"tool", name}, {"success", !isError},
            {"durationMs", static_cast<qint64>(timer.elapsed())},
            {"timestamp", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}});
        while (toolHistory_.size() > 200) toolHistory_.removeFirst();
        return {{"jsonrpc", "2.0"}, {"id", id}, {"result", value}};
    }
    if (method.startsWith(QStringLiteral("notifications/"))) return {};
    return rpcError(id, -32601, QStringLiteral("Method not found: ") + method);
}

QJsonObject McpServer::schema(const QJsonObject &properties, const QStringList &required) const
{
    QJsonArray req; for (const auto &item : required) req.append(item);
    return {{"type", "object"}, {"properties", properties}, {"required", req}};
}

QJsonArray McpServer::toolList() const
{
    QJsonArray tools;
    const auto add = [&tools](const QString &name, const QString &description, const QJsonObject &input) {
        tools.append(QJsonObject{{"name", name}, {"description", description}, {"inputSchema", input}});
    };
    const auto str = [](const QString &description) { return QJsonObject{{"type","string"},{"description",description}}; };
    const auto num = [](const QString &description) { return QJsonObject{{"type","integer"},{"description",description}}; };
    const auto boolean = [](const QString &description) { return QJsonObject{{"type","boolean"},{"description",description}}; };
    add("get_active_file", "获取本机 Qt 编辑器当前活动文件", schema({}));
    add("get_selection", "获取当前文本选区和光标位置", schema({}));
    add("get_open_tabs", "获取本机 Qt 编辑器中打开的文件标签页", schema({}));
    add("get_diagnostics", "获取独立语言服务诊断信息", schema({{"filePath",str("指定文件绝对路径")},{"severity",QJsonObject{{"type","string"},{"enum",QJsonArray{"error","warning","information","hint"}}}}}));
    add("show_diff", "在本机 Qt 编辑器中显示建议更改的差异", schema({{"filePath",str("文件绝对路径")},{"newContent",str("新内容")},{"title",str("可选标题")}}, {"filePath","newContent"}));
    add("get_workspace_info", "读取已选择工作区的路径、目录概况", schema({}));
    add("read_file", "读取工作区文件，可按 0-based 行区间截取", schema({{"filePath",str("相对工作区路径或工作区内绝对路径")},{"startLine",num("包含，0-based")},{"endLine",num("包含，0-based")}}, {"filePath"}));
    add("write_file", "覆盖写入工作区文件", schema({{"filePath",str("工作区内路径")},{"content",str("完整文件内容")},{"createIfMissing",boolean("缺省 true")}}, {"filePath","content"}));
    add("create_file", "新建工作区文件，不覆盖已存在文件", schema({{"filePath",str("工作区内路径")},{"content",str("初始内容")}}, {"filePath"}));
    add("delete_file", "删除文件", schema({{"filePath",str("文件绝对路径")},{"useTrash",boolean("移入回收站")}}, {"filePath"}));
    add("search_workspace_symbols", "通过 LSP 在工作区搜索符号；无语言服务时使用文本索引", schema({{"query",str("要搜索的符号")}}, {"query"}));
    add("open_file", "在本机 Qt 编辑器中打开文件", schema({{"filePath",str("文件绝对路径")},{"line",num("目标行，从 0 开始")},{"character",num("字符位置")},{"preview",boolean("预览模式")}}, {"filePath"}));
    add("close_file", "关闭本机 Qt 编辑器中的文件标签页", schema({{"filePath",str("文件绝对路径")}}, {"filePath"}));
    add("find_references", "通过 LSP 查找符号引用", schema({{"filePath",str("文件绝对路径")},{"line",num("行号，从 0 开始")},{"character",num("字符位置")},{"includeDeclaration",boolean("是否包含声明")}}, {"filePath","line","character"}));
    add("go_to_definition", "通过 LSP 查找符号定义", schema({{"filePath",str("文件绝对路径")},{"line",num("行号，从 0 开始")},{"character",num("字符位置")}}, {"filePath","line","character"}));
    add("get_hover", "获取符号悬停信息", schema({{"filePath",str("文件绝对路径")},{"line",num("行号，从 0 开始")},{"character",num("字符位置")}}, {"filePath","line","character"}));
    add("get_document_symbols", "获取文档符号", schema({{"filePath",str("文件绝对路径")}}, {"filePath"}));
    add("get_code_actions", "获取指定范围内的代码操作", schema({{"filePath",str("文件绝对路径")},{"startLine",num("起始行")},{"startChar",num("起始字符")},{"endLine",num("结束行")},{"endChar",num("结束字符")}}, {"filePath","startLine","startChar","endLine","endChar"}));
    add("apply_code_action", "按索引应用代码操作", schema({{"filePath",str("文件绝对路径")},{"startLine",num("起始行")},{"startChar",num("起始字符")},{"endLine",num("结束行")},{"endChar",num("结束字符")},{"actionIndex",num("代码操作索引")}}, {"filePath","startLine","startChar","endLine","endChar","actionIndex"}));
    add("rename_symbol", "重命名符号及其引用", schema({{"filePath",str("文件绝对路径")},{"line",num("行号")},{"character",num("字符位置")},{"newName",str("新名称")}}, {"filePath","line","character","newName"}));
    add("execute_vscode_command", "兼容命令入口：执行本机编辑器支持的命令", schema({{"command",str("兼容命令 ID")},{"args",QJsonObject{{"type","array"},{"items",QJsonObject{}}}}}, {"command"}));
    add("git_status", "读取当前工作区 Git 状态", schema({}));
    add("git_diff", "读取当前工作区未提交差异", schema({}));
    add("run_terminal_command", "在工作区根目录执行命令，默认超时 30 秒", schema({{"command",str("shell 命令")},{"timeoutMs",num("超时毫秒，上限 120000")}}, {"command"}));
    add("spawn_terminal", "在独立进程终端中启动长时间运行的任务", schema({{"name",str("终端名称")},{"command",str("启动命令")},{"cwd",str("工作目录")}}, {"name"}));
    add("list_terminals", "列出由 MCP 启动的终端进程", schema({}));
    add("read_terminal", "读取托管终端最近输出", schema({{"id",str("终端 ID")},{"lines",num("末尾行数")}}, {"id"}));
    add("write_terminal", "向托管终端发送输入文本", schema({{"id",str("终端 ID")},{"input",str("输入内容")},{"addNewline",boolean("输入后追加换行")}}, {"id","input"}));
    add("kill_terminal", "终止托管终端", schema({{"id",str("终端 ID")}}, {"id"}));
    add("list_directory", "递归列出工作区目录，限制深度和条目数", schema({{"path",str("目录路径，默认工作区")},{"depth",num("递归深度 1-5")},{"maxEntries",num("最多 1500 个")}}));
    add("get_file_info", "获取文件元数据、权限和文本行数", schema({{"path",str("文件路径")}}, {"path"}));
    add("read_multiple_files", "批量读取至多 20 个 UTF-8 文件", schema({{"paths",QJsonObject{{"type","array"},{"items",QJsonObject{{"type","string"}}}}}}, {"paths"}));
    add("create_directory", "在工作区内创建目录", schema({{"path",str("目录路径")}}, {"path"}));
    add("move_file", "移动或重命名文件或目录（不覆盖）", schema({{"source",str("源路径")},{"destination",str("目标路径")}}, {"source","destination"}));
    add("edit_block", "精确局部替换，校验匹配次数后原子写入", schema({{"filePath",str("UTF-8 文件路径")},{"old_string",str("被替换文本")},{"new_string",str("新文本")},{"expected_replacements",num("预期匹配次数，默认 1")}}, {"filePath","old_string","new_string"}));
    add("search_workspace", "搜索文件名或文本，支持正则和过滤，限制扫描量", schema({{"path",str("搜索目录")},{"pattern",str("搜索词")},{"searchType",str("files 或 content，默认 content")},{"regex",boolean("是否正则")},{"ignoreCase",boolean("忽略大小写")},{"filePattern",str("文件通配符")},{"maxResults",num("最大结果数，最多 500")},{"maxFiles",num("最大扫描文件数")}}, {"pattern"}));
    add("get_recent_tool_calls", "查看不含敏感参数的本次运行工具调用摘要", schema({{"maxResults",num("最多返回 200 条")}}));
    add("get_usage_stats", "本次运行 MCP 调用成功/失败和运行时长", schema({}));
    add("list_processes", "列出本机进程及资源使用概况", schema({}));
    add("kill_process", "结束指定系统进程（要求 confirm=true）", schema({{"pid",num("系统 PID")},{"confirm",boolean("必须显式为 true")}}, {"pid","confirm"}));
    add("start_process", "创建持久交互式 shell 会话", schema({{"command",str("初始命令")},{"name",str("会话名")},{"cwd",str("工作目录")}}, {"command"}));
    add("read_process_output", "增量读取会话输出", schema({{"id",str("终端 ID")},{"offset",num("起始字节偏移")},{"lines",num("尾部行数")}}, {"id"}));
    add("interact_with_process", "向持久 shell 发送命令并获取输出", schema({{"id",str("终端 ID")},{"input",str("命令")}}, {"id","input"}));
    add("list_sessions", "查看现有持久 shell 会话", schema({}));
    add("force_terminate", "终止指定 shell 会话", schema({{"id",str("终端 ID")}}, {"id"}));
    add("register_device", "登记 SSH 设备配置（不存储 SSH 私钥和密码）", schema({{"name",str("设备标识")},{"sshHost",str("OpenSSH config alias")},{"root",str("远程工作区 POSIX 绝对路径")}}, {"name","sshHost","root"}));
    add("list_devices", "列出已登记的 SSH 设备与当前 MCP 目标", schema({}));
    add("read_document", "提取 DOCX 段落、XLSX 首表单元格或 PDF 文本；PDF 需 pdftotext", schema({{"filePath",str("文档文件路径")},{"maxChars",num("最大文本字符数")},{"maxCells",num("最大单元格数量")}}, {"filePath"}));
    add("select_device", "切换 MCP 的 SSH 工作区目标（需要 SSH 可连接）", schema({{"name",str("设备标识")}}, {"name"}));
    add("remove_device", "删除 SSH 设备配置", schema({{"name",str("设备标识")}}, {"name"}));
    return tools;
}

QJsonObject McpServer::callEditor(const QString &name, const QJsonObject &args)
{
    if (!editor_) throw QStringLiteral("本机编辑器未初始化");
    QJsonValue value;
    QString error;
    const bool invoked = QMetaObject::invokeMethod(editor_, [&]() {
        try { value = editor_->invokeTool(name, args); }
        catch (const QString &message) { error = message; }
    }, Qt::BlockingQueuedConnection);
    if (!invoked) throw QStringLiteral("无法访问本机编辑器");
    if (!error.isEmpty()) throw error;
    return result(value);
}

QJsonObject McpServer::callLanguage(const QString &name, const QJsonObject &args)
{
    QString filePath = textValue(args.value(QStringLiteral("filePath")));
    if (filePath.isEmpty() && editor_) {
        QMetaObject::invokeMethod(editor_, [&]() { filePath = editor_->activePath(); }, Qt::BlockingQueuedConnection);
    }
    if (filePath.isEmpty()) {
        if (name == QStringLiteral("get_diagnostics")) return result(QJsonArray{});
        throw QStringLiteral("请指定 filePath 或在本机编辑器中打开文件");
    }
    filePath = checkedPath(filePath, true);
    const QString language = LanguageService::languageFor(filePath);
    if (language.isEmpty()) throw QStringLiteral("此文件类型尚无语言服务器配置：") + QFileInfo(filePath).suffix();
    if (!LanguageService::available(language)) throw QStringLiteral("未找到 %1 语言服务器。可安装标准 LSP 服务并加入 PATH。").arg(language);
    LanguageService *service = languages_.value(language);
    if (!service) {
        service = new LanguageService(language, workspace_, this);
        languages_.insert(language, service);
    }
    QString content;
    if (editor_) QMetaObject::invokeMethod(editor_, [&]() { content = editor_->textForPath(filePath); }, Qt::BlockingQueuedConnection);
    if (content.isNull()) {
        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly)) throw QStringLiteral("无法读取文件：") + file.errorString();
        content = QString::fromUtf8(file.readAll());
    }
    if ((name == QStringLiteral("apply_code_action") || name == QStringLiteral("rename_symbol")) && editor_) {
        bool dirty = false;
        QMetaObject::invokeMethod(editor_, [&]() {
            for (const auto &item : editor_->invokeTool(QStringLiteral("get_open_tabs"), {}).toArray()) {
                const QJsonObject tab = item.toObject();
                if (tab.value("path").toString().compare(filePath, Qt::CaseInsensitive) == 0 &&
                    tab.value("isDirty").toBool()) dirty = true;
            }
        }, Qt::BlockingQueuedConnection);
        if (dirty) throw QStringLiteral("请先保存本机编辑器中未保存的更改，再应用语言服务编辑");
    }
    QJsonObject argsWithPath = args;
    argsWithPath.insert(QStringLiteral("filePath"), filePath);
    const QJsonValue value = service->call(name, argsWithPath, content);
    if (name == QStringLiteral("apply_code_action") || name == QStringLiteral("rename_symbol")) {
        if (editor_) QMetaObject::invokeMethod(editor_, [this, filePath]() { editor_->refreshFile(filePath); }, Qt::BlockingQueuedConnection);
    }
    return result(value);
}

QJsonObject McpServer::result(const QJsonValue &value, bool isError) const
{
    QJsonObject contentItem;
    contentItem.insert(QStringLiteral("type"), QStringLiteral("text"));
    QByteArray serialized;
    if (value.isObject()) serialized = QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
    else if (value.isArray()) serialized = QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact);
    else if (value.isNull() || value.isUndefined()) serialized = "null";
    else serialized = QJsonDocument(QJsonObject{{QStringLiteral("value"), value}}).toJson(QJsonDocument::Compact);
    contentItem.insert(QStringLiteral("text"), value.isString() ? value.toString() : QString::fromUtf8(serialized));
    QJsonArray content; content.append(contentItem);
    return {{QStringLiteral("content"), content}, {QStringLiteral("isError"), isError}};
}

QString McpServer::checkedPath(const QString &path, bool mustExist) const
{
    if (workspace_.isEmpty()) throw QStringLiteral("请先在应用中选择工作区");
    const QString candidate = QFileInfo(QDir::isAbsolutePath(path) ? path : QDir(workspace_).filePath(path)).absoluteFilePath();
    const QString cleanRoot = QDir::cleanPath(workspace_);
    const QString clean = QDir::cleanPath(candidate);
    const QString prefix = cleanRoot.endsWith('/') ? cleanRoot : cleanRoot + '/';
#ifdef Q_OS_WIN
    constexpr auto pathCase = Qt::CaseInsensitive;
#else
    constexpr auto pathCase = Qt::CaseSensitive;
#endif
    if (clean.compare(cleanRoot, pathCase) != 0 && !clean.startsWith(prefix, pathCase))
        throw QStringLiteral("路径必须位于当前工作区内");
    const QFileInfo info(clean);
    if (mustExist && !info.exists()) throw QStringLiteral("文件不存在：") + clean;
    QString ancestor = info.exists() ? clean : info.absolutePath();
    while (!QFileInfo::exists(ancestor)) {
        const QString parent = QFileInfo(ancestor).absolutePath();
        if (parent == ancestor) break;
        ancestor = parent;
    }
    const QString resolvedAncestor = QFileInfo(ancestor).canonicalFilePath();
    const QString resolvedFile = info.exists() ? info.canonicalFilePath() : QString{};
    const auto insideRoot = [&cleanRoot, &prefix, pathCase](const QString &candidate) {
        return candidate.compare(cleanRoot, pathCase) == 0 || candidate.startsWith(prefix, pathCase);
    };
    if (!resolvedAncestor.isEmpty() && !insideRoot(resolvedAncestor)) throw QStringLiteral("不允许通过符号链接访问工作区外路径");
    if (!resolvedFile.isEmpty() && !insideRoot(resolvedFile)) throw QStringLiteral("不允许通过符号链接访问工作区外路径");
    return clean;
}

QString McpServer::shellQuote(const QString &value)
{
    QString quoted = value;
    quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

QString McpServer::checkedRemotePath(const QString &path) const
{
    if (sshHost_.isEmpty() || remoteRoot_.isEmpty()) throw QStringLiteral("请填写 SSH 主机别名与远程项目目录");
    static const QRegularExpression hostPattern(QStringLiteral("^[A-Za-z0-9_.@][A-Za-z0-9_.@-]*$"));
    if (!hostPattern.match(sshHost_).hasMatch()) throw QStringLiteral("SSH 主机只能是 ssh config 中的 alias/hostname，不接受命令参数");
    auto normalize = [](const QString &raw, bool allowParent) {
        QStringList parts;
        for (const QString &part : QDir::fromNativeSeparators(raw).split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
            if (part == QStringLiteral(".")) continue;
            if (part == QStringLiteral("..")) {
                if (!allowParent || parts.isEmpty()) throw QStringLiteral("远程路径越出项目目录");
                parts.removeLast();
            } else parts.append(part);
        }
        return QLatin1Char('/') + parts.join(QLatin1Char('/'));
    };
    if (!remoteRoot_.startsWith(QLatin1Char('/')))
        throw QStringLiteral("远程项目目录必须是 POSIX 绝对路径");
    const QString root = normalize(remoteRoot_, false);
    const QString supplied = path.trimmed();
    const QString candidate = normalize(supplied.startsWith(QLatin1Char('/')) ? supplied : root + QLatin1Char('/') + supplied, true);
    if (candidate != root && root != QStringLiteral("/") && !candidate.startsWith(root + QLatin1Char('/')))
        throw QStringLiteral("远程路径必须位于配置的项目目录内");
    // Resolve symlinked ancestors on the remote host; a lexical prefix is not
    // sufficient to prevent escaping through a remote directory symlink.
    const QJsonObject canonical = runRemote(
        QStringLiteral("realpath -m -- %1 && realpath -m -- %2")
            .arg(shellQuote(root), shellQuote(candidate)), {}, 15000);
    if (canonical.value("exitCode").toInt(-1) != 0)
        throw QStringLiteral("远程路径校验失败（需 GNU realpath）：") + canonical.value("stderr").toString();
    const QStringList resolved = canonical.value("stdout").toString().trimmed().split('\n');
    if (resolved.size() != 2) throw QStringLiteral("远程路径校验结果无效");
    const QString canonicalRoot = resolved.at(0).trimmed();
    const QString canonicalPath = resolved.at(1).trimmed();
    if (canonicalPath != canonicalRoot && canonicalRoot != QStringLiteral("/") &&
        !canonicalPath.startsWith(canonicalRoot + QLatin1Char('/')))
        throw QStringLiteral("拒绝通过远程符号链接访问工作区外路径");
    return candidate;
}

QJsonObject McpServer::runRemote(const QString &command, const QByteArray &stdinData, int timeoutMs) const
{
    if (sshHost_.isEmpty()) throw QStringLiteral("未启用 SSH 远程工作区");
    const QString ssh = QStandardPaths::findExecutable(QStringLiteral("ssh"));
    if (ssh.isEmpty()) throw QStringLiteral("系统未找到 OpenSSH ssh.exe，请启用 Windows OpenSSH Client");
    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(ssh, {QStringLiteral("-T"), QStringLiteral("-oBatchMode=yes"),
        QStringLiteral("-oConnectTimeout=10"), sshHost_, command});
    if (!process.waitForStarted(10000)) throw QStringLiteral("无法启动 ssh：") + process.errorString();
    if (!stdinData.isEmpty() && process.write(stdinData) != stdinData.size()) {
        process.kill(); process.waitForFinished(1000);
        throw QStringLiteral("SSH 数据发送失败：") + process.errorString();
    }
    process.closeWriteChannel();
    QByteArray out, err;
    QElapsedTimer elapsed; elapsed.start();
    while (process.state() != QProcess::NotRunning && elapsed.elapsed() < timeoutMs) {
        process.waitForReadyRead(100);
        out += process.readAllStandardOutput();
        err += process.readAllStandardError();
        if (out.size() > 4 * 1024 * 1024 || err.size() > 1024 * 1024) {
            process.kill(); process.waitForFinished(1000);
            throw QStringLiteral("SSH 返回数据超过限制");
        }
    }
    out += process.readAllStandardOutput();
    err += process.readAllStandardError();
    if (process.state() != QProcess::NotRunning) {
        process.kill(); process.waitForFinished(1000);
        throw QStringLiteral("SSH 操作超时（%1 ms）").arg(timeoutMs);
    }
    return {{QStringLiteral("stdout"), QString::fromUtf8(out)},
            {QStringLiteral("stderr"), QString::fromUtf8(err)},
            {QStringLiteral("exitCode"), process.exitCode()}};
}

QJsonObject McpServer::callTool(const QString &name, const QJsonObject &args, bool *isError)
{
    try {
        *isError = false;
        if (name == QStringLiteral("start_process")) {
            QJsonObject normalized = args;
            if (normalized.value("name").toString().isEmpty()) normalized.insert("name", "Shell Session");
            return callTool(QStringLiteral("spawn_terminal"), normalized, isError);
        }
        if (name == QStringLiteral("read_process_output")) return callTool(QStringLiteral("read_terminal"), args, isError);
        if (name == QStringLiteral("list_sessions")) return callTool(QStringLiteral("list_terminals"), args, isError);
        if (name == QStringLiteral("force_terminate")) return callTool(QStringLiteral("kill_terminal"), args, isError);
        if (name == QStringLiteral("interact_with_process")) {
            QJsonObject input = args;
            input.insert("addNewline", true);
            QJsonObject sent = callTool(QStringLiteral("write_terminal"), input, isError);
            if (*isError) return sent;
            QProcess *proc = terminals_.value(args.value("id").toString());
            if (proc) proc->waitForReadyRead(500);
            return callTool(QStringLiteral("read_terminal"), args, isError);
        }
        const QString filePath = textValue(args.value(QStringLiteral("filePath")));
        const bool remote = !sshHost_.isEmpty();
        if (name == QStringLiteral("register_device") || name == QStringLiteral("list_devices") ||
            name == QStringLiteral("select_device") || name == QStringLiteral("remove_device")) {
            QSettings settings;
            QJsonArray devices = QJsonDocument::fromJson(settings.value("devices/sshProfiles").toByteArray()).array();
            const QString deviceName = args.value("name").toString().trimmed();
            int existing = -1;
            for (int i = 0; i < devices.size(); ++i)
                if (devices.at(i).toObject().value("name").toString() == deviceName) { existing = i; break; }
            if (name == QStringLiteral("list_devices")) {
                return result(QJsonObject{{"devices", devices}, {"activeSshHost", sshHost_},
                    {"activeRemoteRoot", remoteRoot_}, {"mode", sshHost_.isEmpty() ? "local" : "ssh"}});
            }
            if (deviceName.isEmpty() || deviceName.size() > 64)
                throw QStringLiteral("设备名称长度应为 1–64 字符");
            if (name == QStringLiteral("register_device")) {
                if (existing >= 0 || devices.size() >= 50) throw QStringLiteral("设备名称已存在或超过 50 个设备限制");
                const QString host = args.value("sshHost").toString().trimmed();
                const QString root = args.value("root").toString().trimmed();
                static const QRegularExpression safeHost(QStringLiteral("^[A-Za-z0-9_.@][A-Za-z0-9_.@-]*$"));
                if (!safeHost.match(host).hasMatch() || !root.startsWith('/') || root.contains('\n') || root.contains('\r') || root.contains(QLatin1String("/../")))
                    throw QStringLiteral("SSH alias 或远程目录不合法");
                devices.append(QJsonObject{{"name", deviceName}, {"sshHost", host}, {"root", root}});
                settings.setValue("devices/sshProfiles", QJsonDocument(devices).toJson(QJsonDocument::Compact));
                settings.sync();
                return result(QJsonObject{{"registered", true}, {"name", deviceName}});
            }
            if (name == QStringLiteral("select_device") && deviceName == QStringLiteral("local")) {
                setRemoteTarget({}, {});
                return result(QJsonObject{{"selected", "local"}, {"mode", "local"}});
            }
            if (existing < 0) throw QStringLiteral("未找到 SSH 设备：") + deviceName;
            if (name == QStringLiteral("remove_device")) {
                const QJsonObject removed = devices.at(existing).toObject();
                if (removed.value("sshHost").toString() == sshHost_ && removed.value("root").toString() == remoteRoot_)
                    throw QStringLiteral("无法删除正在使用的设备，请先切换工作区");
                devices.removeAt(existing);
                settings.setValue("devices/sshProfiles", QJsonDocument(devices).toJson(QJsonDocument::Compact));
                settings.sync();
                return result(QJsonObject{{"removed", true}, {"name", deviceName}});
            }
            const QJsonObject selected = devices.at(existing).toObject();
            const QString previousHost = sshHost_, previousRoot = remoteRoot_;
            setRemoteTarget(selected.value("sshHost").toString(), selected.value("root").toString());
            try { checkedRemotePath(QString{}); }
            catch (...) { setRemoteTarget(previousHost, previousRoot); throw; }
            emit logMessage(QStringLiteral("MCP SSH 设备已切换：%1").arg(deviceName));
            return result(QJsonObject{{"selected", deviceName}, {"sshHost", sshHost_}, {"root", remoteRoot_}});
        }
        if (name == QStringLiteral("kill_process")) {
            const qint64 pid = args.value("pid").toInteger();
            if (!args.value("confirm").toBool() || pid <= 4 || pid == QCoreApplication::applicationPid())
                throw QStringLiteral("结束进程需要 confirm=true 且 PID 必须是有效的非自身进程");
            QProcess proc;
#ifdef Q_OS_WIN
            proc.start(QStringLiteral("taskkill.exe"), {QStringLiteral("/PID"), QString::number(pid), QStringLiteral("/T"), QStringLiteral("/F")});
#else
            proc.start(QStringLiteral("kill"), {QStringLiteral("-TERM"), QString::number(pid)});
#endif
            if (!proc.waitForStarted(3000) || !proc.waitForFinished(5000) || proc.exitCode() != 0)
                throw QStringLiteral("无法结束指定进程：") + QString::fromLocal8Bit(proc.readAllStandardError());
            return result(QJsonObject{{"pid", pid}, {"terminated", true}});
        }
        if (name == QStringLiteral("list_processes")) {
            QProcess proc;
#ifdef Q_OS_WIN
            proc.start(QStringLiteral("tasklist.exe"), {QStringLiteral("/FO"), QStringLiteral("CSV"), QStringLiteral("/NH")});
#else
            proc.start(QStringLiteral("ps"), {QStringLiteral("-eo"), QStringLiteral("pid=,comm=,rss=,%cpu=")});
#endif
            if (!proc.waitForStarted(3000) || !proc.waitForFinished(5000)) {
                proc.kill(); throw QStringLiteral("系统进程查询超时或不可用");
            }
            if (proc.exitCode() != 0) throw QStringLiteral("系统进程查询失败");
            QJsonArray list;
            const QStringList lines = QString::fromLocal8Bit(proc.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
            for (const QString &line : lines) {
                if (list.size() >= 300) break;
#ifdef Q_OS_WIN
                static const QRegularExpression rowPattern(QStringLiteral("^\"([^\"]+)\",\"([0-9]+)\""));
                const auto match = rowPattern.match(line.trimmed());
                if (!match.hasMatch()) continue;
                list.append(QJsonObject{{"name", match.captured(1)}, {"pid", match.captured(2).toLongLong()}});
#else
                const QStringList fields = line.simplified().split(' ');
                if (fields.size() < 4) continue;
                list.append(QJsonObject{{"pid", fields[0].toLongLong()}, {"name", fields[1]},
                    {"memoryKb", fields[2].toLongLong()}, {"cpuPercent", fields[3].toDouble()}});
#endif
            }
            return result(QJsonObject{{"processes", list}, {"truncated", list.size() >= 300}});
        }
        if (name == QStringLiteral("get_usage_stats"))
            return result(QJsonObject{{"successfulCalls", successfulCalls_},
                {"failedCalls", failedCalls_}, {"uptimeMs", uptime_.elapsed()},
                {"historyCount", toolHistory_.size()}});
        if (name == QStringLiteral("get_recent_tool_calls")) {
            const int count = qBound(1, args.value("maxResults").toInt(50), 200);
            QJsonArray entries;
            for (int i = qMax(0, toolHistory_.size() - count); i < toolHistory_.size(); ++i)
                entries.append(toolHistory_.at(i));
            return result(entries);
        }
        if (name == QStringLiteral("read_document")) {
            if (remote) throw QStringLiteral("read_document 目前仅支持本机工作区");
            return result(DocumentTools::read(args, [this](const QString &path, bool mustExist) {
                return checkedPath(path, mustExist);
            }));
        }
        if (WorkspaceTools::supports(name)) {
            if (remote) throw QStringLiteral("该文件操作目前仅支持本机工作区；SSH 模式请使用现有的远程读写工具");
            if (editor_ && name == QStringLiteral("edit_block")) {
                const QString target = checkedPath(args.value("filePath").toString(), true);
                bool dirty = false;
                QMetaObject::invokeMethod(editor_, [&]() {
                    const QJsonArray tabs = editor_->invokeTool(QStringLiteral("get_open_tabs"), {}).toArray();
                    for (const auto &tab : tabs) {
                        if (tab.toObject().value("path").toString().compare(target, Qt::CaseInsensitive) == 0
                            && tab.toObject().value("isDirty").toBool()) dirty = true;
                    }
                }, Qt::BlockingQueuedConnection);
                if (dirty) throw QStringLiteral("编辑器存在未保存的修改；请先保存，避免覆盖");
            }
            const QJsonValue value = WorkspaceTools::call(name, args, workspace_,
                [this](const QString &path, bool mustExist) { return checkedPath(path, mustExist); });
            if (editor_ && (name == QStringLiteral("edit_block") || name == QStringLiteral("move_file"))) {
                const QString refresh = name == QStringLiteral("edit_block")
                    ? checkedPath(args.value("filePath").toString(), true) : QString{};
                if (!refresh.isEmpty()) QMetaObject::invokeMethod(editor_, [this, refresh]() { editor_->refreshFile(refresh); }, Qt::BlockingQueuedConnection);
            }
            return result(value);
        }
        if (remote && (name == QStringLiteral("get_diagnostics") ||
            name == QStringLiteral("find_references") || name == QStringLiteral("go_to_definition") ||
            name == QStringLiteral("get_hover") || name == QStringLiteral("get_document_symbols") ||
            name == QStringLiteral("get_code_actions") || name == QStringLiteral("apply_code_action") || name == QStringLiteral("rename_symbol")))
            throw QStringLiteral("远程 LSP 尚未配置；请在服务器安装对应语言服务器，后续版本支持通过 SSH 启动");
        if (name == QStringLiteral("get_active_file") || name == QStringLiteral("get_selection") ||
            name == QStringLiteral("get_open_tabs")) return callEditor(name, args);
        if (name == QStringLiteral("get_diagnostics") && filePath.isEmpty() && editor_) {
            QJsonArray tabs;
            QMetaObject::invokeMethod(editor_, [&]() { tabs = editor_->invokeTool(QStringLiteral("get_open_tabs"), {}).toArray(); }, Qt::BlockingQueuedConnection);
            QJsonArray all;
            for (const auto &entry : tabs) {
                const QString path = entry.toObject().value(QStringLiteral("path")).toString();
                if (path.isEmpty() || LanguageService::languageFor(path).isEmpty() ||
                    !LanguageService::available(LanguageService::languageFor(path))) continue;
                QJsonObject options = args;
                options.insert(QStringLiteral("filePath"), path);
                const QJsonObject response = callLanguage(name, options);
                const QJsonArray content = response.value(QStringLiteral("content")).toArray();
                if (!content.isEmpty()) {
                    const QJsonArray items = QJsonDocument::fromJson(content.first().toObject().value(QStringLiteral("text")).toString().toUtf8()).array();
                    for (const auto &item : items) all.append(item);
                }
            }
            return result(all);
        }
        if (name == QStringLiteral("get_diagnostics") || name == QStringLiteral("find_references") ||
            name == QStringLiteral("go_to_definition") || name == QStringLiteral("get_hover") ||
            name == QStringLiteral("get_document_symbols") || name == QStringLiteral("get_code_actions") ||
            name == QStringLiteral("apply_code_action") || name == QStringLiteral("rename_symbol"))
            return callLanguage(name, args);
        if (name == QStringLiteral("open_file") || name == QStringLiteral("show_diff") ||
            name == QStringLiteral("close_file")) {
            QJsonObject editorArgs = args;
            editorArgs.insert(QStringLiteral("filePath"), remote ? checkedRemotePath(filePath) : checkedPath(filePath, name != QStringLiteral("close_file")));
            return callEditor(name, editorArgs);
        }
        if (name == QStringLiteral("execute_vscode_command")) {
            const QString command = textValue(args.value(QStringLiteral("command")));
            const QJsonArray values = args.value(QStringLiteral("args")).toArray();
            if (command == QStringLiteral("workbench.action.files.save"))
                return callEditor(QStringLiteral("save_active_file"), {});
            if (command == QStringLiteral("workbench.action.files.openFile") && !values.isEmpty())
                return callEditor(QStringLiteral("open_file"), {{"filePath", remote ? checkedRemotePath(values.first().toString()) : checkedPath(values.first().toString(), true)}});
            if (command == QStringLiteral("workbench.action.closeActiveEditor")) {
                QJsonValue active;
                QString editorError;
                QMetaObject::invokeMethod(editor_, [&]() { active = editor_->invokeTool(QStringLiteral("get_active_file"), {}); }, Qt::BlockingQueuedConnection);
                if (active.isObject()) return callEditor(QStringLiteral("close_file"), {{"filePath", active.toObject().value("path")}});
                return result(QJsonObject{{"closed", 0}});
            }
            throw QStringLiteral("本机编辑器不支持此命令：") + command;
        }
        if (name == QStringLiteral("get_workspace_info")) {
            if (remote) {
                const QString root = checkedRemotePath(QString{});
                const QJsonObject response = runRemote(QStringLiteral("ls -1A -- %1").arg(shellQuote(root)));
                if (response.value("exitCode").toInt() != 0) throw QStringLiteral("远程目录读取失败：") + response.value("stderr").toString();
                QJsonArray entries;
                for (const QString &entry : response.value("stdout").toString().split('\n', Qt::SkipEmptyParts)) {
                    if (entries.size() >= 100) break;
                    entries.append(entry);
                }
                const QString nameValue = QFileInfo(root).fileName();
                return result(QJsonObject{{"workspace",root},{"rootPath",root},{"name",nameValue},
                    {"folders",QJsonArray{QJsonObject{{"name",nameValue},{"path",root}}}},
                    {"entries",entries},{"truncated",response.value("stdout").toString().split('\n', Qt::SkipEmptyParts).size()>100},
                    {"remote",true},{"sshHost",sshHost_}});
            }
            QDir root(workspace_); if (workspace_.isEmpty()) throw QStringLiteral("请先在应用中选择工作区");
            const QFileInfoList entries = root.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::DirsFirst | QDir::Name);
            QJsonArray names; int shown = 0;
            for (const auto &entry : entries) { if (++shown > 100) break; names.append(entry.fileName() + (entry.isDir() ? QStringLiteral("/") : QString{})); }
            return result(QJsonObject{{"workspace",workspace_},{"rootPath",workspace_},{"name",root.dirName()},
                {"folders",QJsonArray{QJsonObject{{"name",root.dirName()},{"path",workspace_}}}},
                {"entries",names},{"truncated",entries.size()>100}});
        }
        if (name == QStringLiteral("read_file")) {
            if (remote) {
                const QString path = checkedRemotePath(filePath);
                const QJsonObject response = runRemote(QStringLiteral("cat -- %1").arg(shellQuote(path)));
                if (response.value("exitCode").toInt() != 0) throw QStringLiteral("远程文件读取失败：") + response.value("stderr").toString();
                QString content = response.value("stdout").toString();
                const QStringList lines = content.split('\n');
                const int start = qMax(0, args.value("startLine").toInt(0));
                const int end = args.value("endLine").isUndefined() ? lines.size()-1 : qMin(lines.size()-1,args.value("endLine").toInt());
                if (!args.value("startLine").isUndefined() || !args.value("endLine").isUndefined()) content = start <= end ? lines.mid(start,end-start+1).join('\n') : QString{};
                return result(QJsonObject{{"content",content},{"lineCount",lines.size()},
                    {"language",QFileInfo(path).suffix()},{"path",path},{"remote",true}});
            }
            const QString path = checkedPath(filePath, true);
            QString content;
            if (editor_) QMetaObject::invokeMethod(editor_, [&]() { content = editor_->textForPath(path); }, Qt::BlockingQueuedConnection);
            if (content.isNull()) {
                QFile f(path);
                if (!f.open(QIODevice::ReadOnly)) throw f.errorString();
                content = QString::fromUtf8(f.read(8 * 1024 * 1024));
            }
            const QStringList lines = content.split('\n');
            const int start = qMax(0, args.value("startLine").toInt(0));
            const int end = args.value("endLine").isUndefined() ? lines.size() - 1 : qMin(lines.size()-1,args.value("endLine").toInt());
            if (!args.value("startLine").isUndefined() || !args.value("endLine").isUndefined()) content = start <= end ? lines.mid(start,end-start+1).join('\n') : QString{};
            return result(QJsonObject{{"content",content},{"lineCount",lines.size()},{"language",QFileInfo(path).suffix()}});
        }
        if (name == QStringLiteral("write_file") || name == QStringLiteral("create_file")) {
            if (remote) {
                const QString path = checkedRemotePath(filePath);
                const qsizetype parentEnd = path.lastIndexOf(QLatin1Char('/'));
                const QString parent = parentEnd <= 0 ? QStringLiteral("/") : path.left(parentEnd);
                const QJsonObject status = runRemote(QStringLiteral("test -e %1").arg(shellQuote(path)));
                const bool exists = status.value("exitCode").toInt() == 0;
                if (name == QStringLiteral("create_file")) {
                    if (exists) throw QStringLiteral("远程文件已存在：") + path;
                } else if (!exists && !args.value("createIfMissing").toBool(true)) {
                    throw QStringLiteral("远程文件不存在，且 createIfMissing=false");
                }
                const QString command = QStringLiteral("mkdir -p -- %1 && (%2cat > %3)")
                    .arg(shellQuote(parent), name == QStringLiteral("create_file") ? QStringLiteral("set -C; ") : QString{}, shellQuote(path));
                const QByteArray bytes = textValue(args.value("content")).toUtf8();
                const QJsonObject saved = runRemote(command, bytes);
                if (saved.value("exitCode").toInt() != 0) throw QStringLiteral("远程文件写入失败：") + saved.value("stderr").toString();
                return result(QJsonObject{{"path",path},{"bytesWritten",bytes.size()},
                    {"created",!exists},{"remote",true}});
            }
            const QString path = checkedPath(filePath); const bool exists = QFileInfo::exists(path);
            if (name == QStringLiteral("create_file") && exists) throw QStringLiteral("文件已存在：") + path;
            if (name == QStringLiteral("write_file") && exists == false && !args.value("createIfMissing").toBool(true)) throw QStringLiteral("文件不存在，且 createIfMissing=false");
            QDir().mkpath(QFileInfo(path).absolutePath()); QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly)) throw file.errorString();
            const QByteArray content = textValue(args.value("content")).toUtf8();
            if (file.write(content) != content.size() || !file.commit()) throw file.errorString();
            if (editor_) QMetaObject::invokeMethod(editor_, [this, path]() { editor_->refreshFile(path); }, Qt::BlockingQueuedConnection);
            return result(QJsonObject{{"path",path},{"bytesWritten",content.size()},{"created",!exists}});
        }
        if (name == QStringLiteral("delete_file")) {
            if (remote) {
                const QString path = checkedRemotePath(filePath);
                const QString command = args.value("useTrash").toBool(false)
                    ? QStringLiteral("gio trash -- %1").arg(shellQuote(path))
                    : QStringLiteral("rm -f -- %1").arg(shellQuote(path));
                const QJsonObject removed = runRemote(command);
                if (removed.value("exitCode").toInt() != 0) throw QStringLiteral("远程文件删除失败：") + removed.value("stderr").toString();
                return result(QJsonObject{{"deleted",true},{"path",path},{"remote",true}});
            }
            const QString path = checkedPath(filePath,true);
            const bool useTrash = args.value("useTrash").toBool(false);
            if (!QFileInfo(path).isFile() || !(useTrash ? QFile::moveToTrash(path) : QFile::remove(path)))
                throw QStringLiteral("无法删除文件：") + path;
            return result(QJsonObject{{"deleted",true},{"path",path}});
        }
        if (name == QStringLiteral("spawn_terminal")) {
            if (workspace_.isEmpty() && !remote) throw QStringLiteral("请先在应用中选择工作区");
            if (terminals_.size() >= 32) throw QStringLiteral("最多可管理 32 个终端");
            if (remote) {
                const QString cwd = checkedRemotePath(textValue(args.value("cwd")));
                const QString command = QStringLiteral("cd %1 && exec \"${SHELL:-/bin/bash}\"").arg(shellQuote(cwd));
                const QString ssh = QStandardPaths::findExecutable(QStringLiteral("ssh"));
                if (ssh.isEmpty()) throw QStringLiteral("系统未找到 OpenSSH ssh.exe");
                const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                auto *proc = new QProcess(this);
                proc->setProcessChannelMode(QProcess::MergedChannels);
                proc->setProperty("terminalName", textValue(args.value("name")));
                proc->setProperty("terminalOutput", QByteArray{});
                proc->setProperty("terminalTotalBytes", static_cast<qint64>(0));
                proc->setProperty("createdAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
                proc->setProperty("remote", true);
                proc->setProperty("cwd", cwd);
                connect(proc, &QProcess::readyRead, proc, [proc]() {
                    QByteArray output = proc->property("terminalOutput").toByteArray();
                    const QByteArray chunk = proc->readAll();
                    proc->setProperty("terminalTotalBytes", proc->property("terminalTotalBytes").toLongLong() + chunk.size());
                    output += chunk; constexpr qsizetype cap = 512 * 1024;
                    if (output.size() > cap) output = output.right(cap);
                    proc->setProperty("terminalOutput", output);
                });
                terminals_.insert(id, proc);
                proc->start(ssh, {QStringLiteral("-tt"), QStringLiteral("-oConnectTimeout=10"), sshHost_, command});
                if (!proc->waitForStarted(10000)) { terminals_.remove(id); proc->deleteLater(); throw QStringLiteral("远程终端 SSH 启动失败：") + proc->errorString(); }
                const QString initial = textValue(args.value("command"));
                if (!initial.isEmpty()) proc->write(initial.toUtf8() + "\n");
                return result(QJsonObject{{"id",id},{"terminalId",id},{"name",proc->property("terminalName").toString()},
                    {"cwd",cwd},{"pid",static_cast<qint64>(proc->processId())},{"remote",true},{"sshHost",sshHost_}});
            }
            QString cwd=textValue(args.value("cwd")); if(cwd.isEmpty()) cwd=workspace_;
            cwd = checkedPath(cwd, true);
            const QFileInfo cwdInfo(cwd); if(!cwdInfo.exists() || !cwdInfo.isDir()) throw QStringLiteral("终端工作目录不存在：")+cwd;
            const QString id=QUuid::createUuid().toString(QUuid::WithoutBraces);
            auto *proc=new QProcess(this); proc->setWorkingDirectory(cwdInfo.canonicalFilePath()); proc->setProcessChannelMode(QProcess::MergedChannels);
            proc->setProperty("terminalName",textValue(args.value("name")));
            proc->setProperty("terminalOutput",QByteArray{});
            proc->setProperty("terminalTotalBytes", static_cast<qint64>(0));
            proc->setProperty("createdAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
            connect(proc,&QProcess::readyRead,proc,[proc]() {
                QByteArray output=proc->property("terminalOutput").toByteArray();
                const QByteArray chunk = proc->readAll();
                proc->setProperty("terminalTotalBytes", proc->property("terminalTotalBytes").toLongLong() + chunk.size());
                output+=chunk; constexpr qsizetype cap=512*1024;
                if(output.size()>cap) output=output.right(cap);
                proc->setProperty("terminalOutput",output);
            });
            terminals_.insert(id,proc);
#ifdef Q_OS_WIN
            proc->start(QStringLiteral("cmd.exe"),{QStringLiteral("/d"),QStringLiteral("/q")});
#else
            proc->start(QStringLiteral("/bin/sh"));
#endif
            if(!proc->waitForStarted(5000)) { terminals_.remove(id); proc->deleteLater(); throw QStringLiteral("终端启动失败：")+proc->errorString(); }
            const QString command=textValue(args.value("command"));
            if(!command.isEmpty()) { proc->write(command.toUtf8()); proc->write("\n"); }
            return result(QJsonObject{{"id",id},{"terminalId",id},{"name",proc->property("terminalName").toString()},
                                      {"cwd",cwdInfo.canonicalFilePath()},{"pid",static_cast<qint64>(proc->processId())}});
        }
        if (name == QStringLiteral("list_terminals")) {
            QJsonArray list;
            for(auto it=terminals_.cbegin();it!=terminals_.cend();++it) {
                QProcess *proc=it.value();
                list.append(QJsonObject{{"id",it.key()},{"terminalId",it.key()},{"name",proc->property("terminalName").toString()},
                    {"cwd",proc->property("remote").toBool() ? proc->property("cwd").toString() : proc->workingDirectory()},
                    {"remote",proc->property("remote").toBool()},{"alive",proc->state()!=QProcess::NotRunning},
                    {"running",proc->state()!=QProcess::NotRunning},{"createdAt",proc->property("createdAt").toString()},
                    {"logSize",proc->property("terminalOutput").toByteArray().size()},
                    {"pid",static_cast<qint64>(proc->processId())},{"exitCode",proc->exitCode()}});
            }
            return result(list);
        }
        if (name == QStringLiteral("read_terminal")) {
            const QString id=textValue(args.value("id").isString()?args.value("id"):args.value("terminalId")); QProcess *proc=terminals_.value(id,nullptr);
            if(!proc) throw QStringLiteral("终端不存在：")+id;
            const QByteArray retained = proc->property("terminalOutput").toByteArray();
            const qint64 total = proc->property("terminalTotalBytes").toLongLong();
            const qint64 first = qMax(static_cast<qint64>(0), total - retained.size());
            const qint64 offset = args.value("offset").isUndefined() ? first
                : qBound(first, args.value("offset").toInteger(), total);
            QString output = QString::fromUtf8(retained.mid(static_cast<qsizetype>(offset - first)));
            const int lines=qBound(1,args.value("lines").toInt(200),5000);
            const QStringList split=output.split('\n'); if(split.size()>lines) output=split.mid(split.size()-lines).join('\n');
            return result(QJsonObject{{"id",id},{"terminalId",id},{"output",output},{"alive",proc->state()!=QProcess::NotRunning},
                                      {"running",proc->state()!=QProcess::NotRunning},{"totalBytes",total},
                                      {"nextOffset",total},{"retainedFrom",first},
                                      {"truncated",!args.value("offset").isUndefined() && args.value("offset").toInteger() < first}});
        }
        if (name == QStringLiteral("write_terminal")) {
            const QString id=textValue(args.value("id").isString()?args.value("id"):args.value("terminalId")); QProcess *proc=terminals_.value(id,nullptr);
            if(!proc || proc->state()==QProcess::NotRunning) throw QStringLiteral("终端未运行：")+id;
            QByteArray input=textValue(args.value("input")).toUtf8(); if(args.value("addNewline").toBool(true) && !input.endsWith('\n')) input.append('\n');
            const qint64 written=proc->write(input); if(written<0) throw proc->errorString();
            return result(QJsonObject{{"id",id},{"terminalId",id},{"bytesWritten",written}});
        }
        if (name == QStringLiteral("kill_terminal")) {
            const QString id=textValue(args.value("id").isString()?args.value("id"):args.value("terminalId")); QProcess *proc=terminals_.take(id);
            if(!proc) throw QStringLiteral("终端不存在：")+id;
            if(proc->state()!=QProcess::NotRunning) { proc->terminate(); if(!proc->waitForFinished(1000)) proc->kill(); }
            proc->deleteLater(); return result(QJsonObject{{"terminalId",id},{"killed",true}});
        }
        if (name == QStringLiteral("search_workspace_symbols")) {
            const QString query = textValue(args.value("query")); if (query.isEmpty()) throw QStringLiteral("query 不能为空");
            if (remote) {
                const QString root = checkedRemotePath(QString{});
                const QString command = QStringLiteral("grep -RInI --exclude-dir=.git --exclude-dir=node_modules --exclude-dir=build -- %1 %2 2>/dev/null | head -200")
                    .arg(shellQuote(query), shellQuote(root));
                const QJsonObject response = runRemote(command, {}, 60000);
                QJsonArray matches;
                for (const QString &line : response.value("stdout").toString().split('\n', Qt::SkipEmptyParts)) {
                    const qsizetype first = line.indexOf(':');
                    const qsizetype second = first < 0 ? -1 : line.indexOf(':', first + 1);
                    if (first < 0 || second < 0) continue;
                    bool ok = false; const int lineNumber = line.mid(first + 1, second - first - 1).toInt(&ok);
                    matches.append(QJsonObject{{"name",line.mid(second + 1).trimmed().left(100)}, {"kind","TextMatch"},
                        {"filePath",line.left(first)},{"startLine",ok ? qMax(0,lineNumber-1) : 0}, {"containerName",QJsonValue::Null}});
                }
                return result(matches);
            }
            QJsonArray symbols;
            for (auto *service : languages_) {
                try {
                    for (const auto &item : service->workspaceSymbols(query).toArray()) symbols.append(item);
                } catch (const QString &) { /* Other running servers can still provide results. */ }
            }
            if (!symbols.isEmpty()) return result(symbols);
            QJsonArray matches; QDirIterator it(workspace_, {"*.h","*.hpp","*.c","*.cc","*.cpp","*.cxx","*.ts","*.js","*.py","*.rs","*.go","*.java","*.cs","*.qml"}, QDir::Files, QDirIterator::Subdirectories);
            int visited=0, found=0; const QString needle = query.toCaseFolded();
            while (it.hasNext() && visited++ < 5000 && found < 200) {
                const QString path=it.next(); const QString rel=QDir(workspace_).relativeFilePath(path);
                if (rel.startsWith(QStringLiteral(".git/")) || rel.startsWith(QStringLiteral("node_modules/")) || rel.startsWith(QStringLiteral("build/"))) continue;
                QFile f(path); if (!f.open(QIODevice::ReadOnly)) continue;
                const QStringList lines=QString::fromUtf8(f.read(1024*1024)).split('\n');
                for (qsizetype i=0;i<lines.size() && found<200;++i) if (lines[i].contains(needle,Qt::CaseInsensitive)) {
                    matches.append(QJsonObject{{"name",lines[i].trimmed().left(100)}, {"kind","TextMatch"},
                        {"filePath",path},{"startLine",static_cast<int>(i)},{"containerName",QJsonValue::Null}}); ++found;
                }
            }
            return result(matches);
        }
        if (name == QStringLiteral("git_status") || name == QStringLiteral("git_diff") || name == QStringLiteral("run_terminal_command")) {
            QString command;
            if (name == QStringLiteral("git_status")) command = QStringLiteral("git status --short --branch");
            else if (name == QStringLiteral("git_diff")) command = QStringLiteral("git diff --no-ext-diff --");
            else command = textValue(args.value("command"));
            if (command.isEmpty()) throw QStringLiteral("command 不能为空");
            const int timeout=qBound(1000,args.value("timeoutMs").toInt(30000),120000);
            if (remote) {
                const QString cwd = checkedRemotePath(textValue(args.value("cwd")));
                const QJsonObject response = runRemote(QStringLiteral("cd %1 && bash -lc %2")
                    .arg(shellQuote(cwd), shellQuote(command)), {}, timeout);
                return result(QJsonObject{{"stdout",response.value("stdout")},{"stderr",response.value("stderr")},
                    {"exitCode",response.value("exitCode")},{"truncated",false},{"remote",true},{"sshHost",sshHost_}});
            }
            QString workingDirectory=textValue(args.value("cwd")); if(workingDirectory.isEmpty()) workingDirectory=workspace_;
            workingDirectory = checkedPath(workingDirectory, true);
            if (!QFileInfo(workingDirectory).isDir()) throw QStringLiteral("工作目录不存在：")+workingDirectory;
#ifdef Q_OS_WIN
            QProcess proc; if(!workingDirectory.isEmpty()) proc.setWorkingDirectory(workingDirectory); proc.setProcessChannelMode(QProcess::SeparateChannels);
            proc.start(QStringLiteral("cmd.exe"), {QStringLiteral("/d"),QStringLiteral("/s"),QStringLiteral("/c"),command});
#else
            QProcess proc; if(!workingDirectory.isEmpty()) proc.setWorkingDirectory(workingDirectory); proc.setProcessChannelMode(QProcess::SeparateChannels);
            proc.start(QStringLiteral("/bin/sh"), {QStringLiteral("-lc"),command});
#endif
            if (!proc.waitForStarted(5000)) throw QStringLiteral("命令启动失败：") + proc.errorString();
            QByteArray stdoutData, stderrData;
            bool outputTruncated = false;
            const auto appendBounded = [&outputTruncated](QByteArray &target, const QByteArray &chunk) {
                constexpr qsizetype limit = 1024 * 1024;
                const qsizetype room = limit - target.size();
                if (room > 0) target.append(chunk.constData(), qMin(room, chunk.size()));
                if (chunk.size() > room) outputTruncated = true;
            };
            QElapsedTimer elapsed; elapsed.start();
            while (proc.state() != QProcess::NotRunning && elapsed.elapsed() < timeout) {
                proc.waitForReadyRead(100);
                appendBounded(stdoutData,proc.readAllStandardOutput());
                appendBounded(stderrData,proc.readAllStandardError());
            }
            appendBounded(stdoutData,proc.readAllStandardOutput());
            appendBounded(stderrData,proc.readAllStandardError());
            if (proc.state() != QProcess::NotRunning) { proc.kill(); proc.waitForFinished(1000); throw QStringLiteral("命令超时（%1 ms）").arg(timeout); }
            return result(QJsonObject{{"stdout",QString::fromUtf8(stdoutData)},
                                      {"stderr",QString::fromUtf8(stderrData)},
                                      {"exitCode",proc.exitCode()}, {"truncated",outputTruncated}});
        }
        static const QStringList languageTools = {"get_diagnostics","find_references","go_to_definition",
            "get_hover","get_document_symbols","get_code_actions","apply_code_action","rename_symbol"};
        if (languageTools.contains(name)) throw QStringLiteral("未找到可用于当前语言的本机语言服务");
        throw QStringLiteral("未知工具：") + name;
    } catch (const QString &error) { *isError=true; return result(error,true); }
      catch (const std::exception &error) { *isError=true; return result(QString::fromUtf8(error.what()),true); }
}
