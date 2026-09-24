#include "mcpserver.h"
#include "editorbridge.h"
#include "languageservice.h"

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
    if (headerEnd < 0) { socket->setProperty("requestBuffer", buffer); return; }
    qsizetype bodyLength = 0;
    const QByteArray header = buffer.left(headerEnd);
    for (const QByteArray &line : header.split('\n')) {
        const QByteArray low = line.trimmed().toLower();
        if (low.startsWith("content-length:")) bodyLength = low.mid(15).trimmed().toLongLong();
    }
    if (buffer.size() < headerEnd + 4 + bodyLength) {
        socket->setProperty("requestBuffer", buffer); return;
    }
    socket->setProperty("requestBuffer", QByteArray{});
    disconnect(socket, &QTcpSocket::readyRead, this, &McpServer::readRequest);
    handleRequest(socket, buffer);
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
    if (path == "/health") {
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
        {413,"Payload Too Large"},{500,"Internal Server Error"}};
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
        const auto value = callTool(name,
                                    params.value(QStringLiteral("arguments")).toObject(), &isError);
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
    if (clean != cleanRoot && !clean.startsWith(prefix, Qt::CaseInsensitive)) throw QStringLiteral("路径必须位于当前工作区内");
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
    const auto insideRoot = [&cleanRoot, &prefix](const QString &candidate) {
        return candidate.compare(cleanRoot, Qt::CaseInsensitive) == 0 || candidate.startsWith(prefix, Qt::CaseInsensitive);
    };
    if (!resolvedAncestor.isEmpty() && !insideRoot(resolvedAncestor)) throw QStringLiteral("不允许通过符号链接访问工作区外路径");
    if (!resolvedFile.isEmpty() && !insideRoot(resolvedFile)) throw QStringLiteral("不允许通过符号链接访问工作区外路径");
    return clean;
}

QJsonObject McpServer::callTool(const QString &name, const QJsonObject &args, bool *isError)
{
    try {
        *isError = false;
        const QString filePath = textValue(args.value(QStringLiteral("filePath")));
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
            editorArgs.insert(QStringLiteral("filePath"), checkedPath(filePath, name != QStringLiteral("close_file")));
            return callEditor(name, editorArgs);
        }
        if (name == QStringLiteral("execute_vscode_command")) {
            const QString command = textValue(args.value(QStringLiteral("command")));
            const QJsonArray values = args.value(QStringLiteral("args")).toArray();
            if (command == QStringLiteral("workbench.action.files.save"))
                return callEditor(QStringLiteral("save_active_file"), {});
            if (command == QStringLiteral("workbench.action.files.openFile") && !values.isEmpty())
                return callEditor(QStringLiteral("open_file"), {{"filePath", checkedPath(values.first().toString(), true)}});
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
            QDir root(workspace_); if (workspace_.isEmpty()) throw QStringLiteral("请先在应用中选择工作区");
            const QFileInfoList entries = root.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::DirsFirst | QDir::Name);
            QJsonArray names; int shown = 0;
            for (const auto &entry : entries) { if (++shown > 100) break; names.append(entry.fileName() + (entry.isDir() ? QStringLiteral("/") : QString{})); }
            return result(QJsonObject{{"workspace",workspace_},{"rootPath",workspace_},{"name",root.dirName()},
                {"folders",QJsonArray{QJsonObject{{"name",root.dirName()},{"path",workspace_}}}},
                {"entries",names},{"truncated",entries.size()>100}});
        }
        if (name == QStringLiteral("read_file")) {
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
            const QString path = checkedPath(filePath,true);
            const bool useTrash = args.value("useTrash").toBool(false);
            if (!QFileInfo(path).isFile() || !(useTrash ? QFile::moveToTrash(path) : QFile::remove(path)))
                throw QStringLiteral("无法删除文件：") + path;
            return result(QJsonObject{{"deleted",true},{"path",path}});
        }
        if (name == QStringLiteral("spawn_terminal")) {
            if (workspace_.isEmpty()) throw QStringLiteral("请先在应用中选择工作区");
            if (terminals_.size() >= 32) throw QStringLiteral("最多可管理 32 个终端");
            QString cwd=textValue(args.value("cwd")); if(cwd.isEmpty()) cwd=workspace_;
            const QFileInfo cwdInfo(cwd); if(!cwdInfo.exists() || !cwdInfo.isDir()) throw QStringLiteral("终端工作目录不存在：")+cwd;
            const QString id=QUuid::createUuid().toString(QUuid::WithoutBraces);
            auto *proc=new QProcess(this); proc->setWorkingDirectory(cwdInfo.canonicalFilePath()); proc->setProcessChannelMode(QProcess::MergedChannels);
            proc->setProperty("terminalName",textValue(args.value("name")));
            proc->setProperty("terminalOutput",QByteArray{});
            connect(proc,&QProcess::readyRead,proc,[proc]() {
                QByteArray output=proc->property("terminalOutput").toByteArray();
                output+=proc->readAll(); constexpr qsizetype cap=512*1024;
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
                    {"cwd",proc->workingDirectory()},{"alive",proc->state()!=QProcess::NotRunning},
                    {"running",proc->state()!=QProcess::NotRunning},{"createdAt",QDateTime::currentMSecsSinceEpoch()},
                    {"logSize",proc->property("terminalOutput").toByteArray().size()},
                    {"pid",static_cast<qint64>(proc->processId())},{"exitCode",proc->exitCode()}});
            }
            return result(list);
        }
        if (name == QStringLiteral("read_terminal")) {
            const QString id=textValue(args.value("id").isString()?args.value("id"):args.value("terminalId")); QProcess *proc=terminals_.value(id,nullptr);
            if(!proc) throw QStringLiteral("终端不存在：")+id;
            QString output=QString::fromUtf8(proc->property("terminalOutput").toByteArray());
            const int lines=qBound(1,args.value("lines").toInt(200),5000);
            const QStringList split=output.split('\n'); if(split.size()>lines) output=split.mid(split.size()-lines).join('\n');
            return result(QJsonObject{{"id",id},{"terminalId",id},{"output",output},{"alive",proc->state()!=QProcess::NotRunning},
                                      {"running",proc->state()!=QProcess::NotRunning},{"totalBytes",proc->property("terminalOutput").toByteArray().size()}});
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
            QString workingDirectory=textValue(args.value("cwd")); if(workingDirectory.isEmpty()) workingDirectory=workspace_;
            if(!workingDirectory.isEmpty() && (!QFileInfo(workingDirectory).exists() || !QFileInfo(workingDirectory).isDir())) throw QStringLiteral("工作目录不存在：")+workingDirectory;
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
