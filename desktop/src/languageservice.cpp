#include "languageservice.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <algorithm>

namespace {
QString uriFor(const QString &path) { return QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()).toString(QUrl::FullyEncoded); }
QString pathFor(const QString &uri) { return QFileInfo(QUrl(uri).toLocalFile()).absoluteFilePath(); }
QJsonObject pos(int line, int character) { return {{"line", line}, {"character", character}}; }
int offsetAt(const QString &text, const QJsonObject &position)
{
    const int line = qMax(0, position.value("line").toInt());
    const int character = qMax(0, position.value("character").toInt());
    int offset = 0;
    for (int i = 0; i < line; ++i) {
        const int next = text.indexOf('\n', offset);
        if (next < 0) return text.size();
        offset = next + 1;
    }
    const int next = text.indexOf('\n', offset);
    return qMin(offset + character, next < 0 ? text.size() : next);
}
QStringList commandFor(const QString &language)
{
    QSettings settings;
    const QString override = settings.value(QStringLiteral("languageServers/") + language).toString().trimmed();
    if (!override.isEmpty()) return QProcess::splitCommand(override);
    if (language == "cpp" || language == "c") {
        const QString bundled = QCoreApplication::applicationDirPath() + QStringLiteral("/lsp/clangd/bin/clangd.exe");
        return {QFileInfo::exists(bundled) ? bundled : QStringLiteral("clangd"), QStringLiteral("--background-index")};
    }
    if (language == "python") {
        if (!QStandardPaths::findExecutable("pylsp").isEmpty()) return {"pylsp"};
        return {"pyright-langserver", "--stdio"};
    }
    if (language == "typescript" || language == "javascript") return {"typescript-language-server", "--stdio"};
    if (language == "rust") return {"rust-analyzer"};
    if (language == "go") return {"gopls"};
    if (language == "java") return {"jdtls"};
    if (language == "csharp") return {"csharp-ls"};
    return {};
}
QString kindName(int kind)
{
    static const QStringList names = {"File","Module","Namespace","Package","Class","Method","Property",
        "Field","Constructor","Enum","Interface","Function","Variable","Constant","String","Number",
        "Boolean","Array","Object","Key","Null","EnumMember","Struct","Event","Operator","TypeParameter"};
    return kind > 0 && kind <= names.size() ? names[kind-1] : QStringLiteral("Unknown");
}
QJsonObject symbol(const QJsonObject &input)
{
    const QJsonObject range = input.value("range").toObject();
    const QJsonArray children = input.value("children").toArray();
    QJsonArray mapped;
    for (const auto &child : children) mapped.append(symbol(child.toObject()));
    return {{"name", input.value("name")}, {"kind", kindName(input.value("kind").toInt())},
        {"startLine", range.value("start").toObject().value("line")},
        {"endLine", range.value("end").toObject().value("line")},
        {"detail", input.value("detail")}, {"children", mapped}};
}
}

LanguageService::LanguageService(QString language, QString workspace, QObject *parent)
    : QObject(parent), language_(std::move(language)), workspace_(std::move(workspace))
{
    process_.setWorkingDirectory(workspace_);
    process_.setProcessChannelMode(QProcess::SeparateChannels);
    connect(&process_, &QProcess::readyReadStandardOutput, this, &LanguageService::readMessages);
}

LanguageService::~LanguageService()
{
    if (process_.state() != QProcess::NotRunning) {
        try { notify(QStringLiteral("exit"), {}); } catch (...) {}
        process_.terminate();
        if (!process_.waitForFinished(1000)) { process_.kill(); process_.waitForFinished(1000); }
    }
}

QString LanguageService::languageFor(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (QStringList{"c"}.contains(suffix)) return "c";
    if (QStringList{"cpp","cc","cxx","h","hpp","hh"}.contains(suffix)) return "cpp";
    if (suffix == "py") return "python";
    if (QStringList{"ts","tsx"}.contains(suffix)) return "typescript";
    if (QStringList{"js","jsx","mjs"}.contains(suffix)) return "javascript";
    if (suffix == "rs") return "rust";
    if (suffix == "go") return "go";
    if (suffix == "java") return "java";
    if (suffix == "cs") return "csharp";
    return {};
}

bool LanguageService::available(const QString &language)
{
    const QStringList command = commandFor(language);
    return !command.isEmpty() && (QFileInfo(command.first()).isExecutable() ||
        !QStandardPaths::findExecutable(command.first()).isEmpty());
}

void LanguageService::start()
{
    if (process_.state() != QProcess::NotRunning) return;
    const QStringList command = commandFor(language_);
    if (command.isEmpty()) throw QStringLiteral("不支持的语言：") + language_;
    const QString executable = QFileInfo(command.first()).isExecutable() ? command.first() : QStandardPaths::findExecutable(command.first());
    if (executable.isEmpty()) throw QStringLiteral("未找到 %1 语言服务器；请安装并加入 PATH，或设置 languageServers/%2").arg(command.first(), language_);
    process_.start(executable, command.mid(1));
    if (!process_.waitForStarted(5000)) throw QStringLiteral("语言服务器启动失败：") + process_.errorString();
    const QString rootUri = uriFor(workspace_);
    const QJsonObject capabilities{{"workspace", QJsonObject{{"workspaceFolders", true}, {"configuration", true}}},
        {"textDocument", QJsonObject{{"publishDiagnostics", QJsonObject{}}, {"documentSymbol", QJsonObject{{"hierarchicalDocumentSymbolSupport", true}}},
            {"codeAction", QJsonObject{{"resolveSupport", QJsonObject{{"properties", QJsonArray{"edit","command"}}}}}}}}};
    request(QStringLiteral("initialize"), {{"processId", static_cast<qint64>(QCoreApplication::applicationPid())},
        {"rootUri", rootUri}, {"rootPath", workspace_},
        {"workspaceFolders", QJsonArray{QJsonObject{{"uri", rootUri}, {"name", QFileInfo(workspace_).fileName()}}}},
        {"capabilities", capabilities}, {"clientInfo", QJsonObject{{"name", "qt-mcp-standalone"}, {"version", "3.0.0"}}}}, 15000);
    initialized_ = true;
    notify(QStringLiteral("initialized"), {});
}

void LanguageService::send(const QJsonObject &message)
{
    if (process_.state() == QProcess::NotRunning) throw QStringLiteral("语言服务器未运行");
    const QByteArray body = QJsonDocument(message).toJson(QJsonDocument::Compact);
    const QByteArray frame = "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
    if (process_.write(frame) != frame.size()) throw QStringLiteral("写入语言服务器失败");
    process_.waitForBytesWritten(1000);
}

void LanguageService::notify(const QString &method, const QJsonObject &params)
{
    send({{"jsonrpc", "2.0"}, {"method", method}, {"params", params}});
}

QJsonValue LanguageService::request(const QString &method, const QJsonObject &params, int timeoutMs)
{
    if (process_.state() == QProcess::NotRunning) start();
    const int id = nextId_++;
    send({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});
    QElapsedTimer timer; timer.start();
    while (!replies_.contains(id) && timer.elapsed() < timeoutMs && process_.state() != QProcess::NotRunning) {
        process_.waitForReadyRead(qMin(100, timeoutMs - static_cast<int>(timer.elapsed())));
        readMessages();
    }
    if (!replies_.contains(id)) throw QStringLiteral("语言服务器请求超时或已退出：") + method +
        QStringLiteral("；stderr: ") + QString::fromUtf8(process_.readAllStandardError()).left(1000);
    const QJsonObject reply = replies_.take(id);
    if (reply.contains("error")) throw QStringLiteral("语言服务器错误：") + reply.value("error").toObject().value("message").toString();
    return reply.value("result");
}

void LanguageService::readMessages()
{
    buffer_ += process_.readAllStandardOutput();
    while (true) {
        const qsizetype headerEnd = buffer_.indexOf("\r\n\r\n");
        if (headerEnd < 0) return;
        qsizetype length = -1;
        for (const QByteArray &line : buffer_.left(headerEnd).split('\n')) {
            const QByteArray trimmed = line.trimmed();
            if (trimmed.toLower().startsWith("content-length:")) length = trimmed.mid(15).trimmed().toLongLong();
        }
        if (length < 0 || length > 32 * 1024 * 1024) { buffer_.clear(); return; }
        if (buffer_.size() < headerEnd + 4 + length) return;
        const QByteArray body = buffer_.mid(headerEnd + 4, length);
        buffer_.remove(0, headerEnd + 4 + length);
        const QJsonObject message = QJsonDocument::fromJson(body).object();
        if (message.value("id").isDouble() && !message.contains("method")) {
            replies_.insert(message.value("id").toInt(), message);
            continue;
        }
        const QString method = message.value("method").toString();
        if (method == QStringLiteral("textDocument/publishDiagnostics")) {
            const QJsonObject params = message.value("params").toObject();
            diagnostics_.insert(pathFor(params.value("uri").toString()), params.value("diagnostics").toArray());
        } else if (message.contains("id")) {
            const QJsonValue answer = method == QStringLiteral("workspace/configuration") ? QJsonValue(QJsonArray{}) : QJsonValue(QJsonValue::Null);
            send({{"jsonrpc", "2.0"}, {"id", message.value("id")}, {"result", answer}});
        }
    }
}

void LanguageService::prepareDocument(const QString &path, const QString &content)
{
    if (process_.state() == QProcess::NotRunning) start();
    const QString absolute = QFileInfo(path).absoluteFilePath();
    const QString uri = uriFor(absolute);
    if (!openDocuments_.contains(absolute)) {
        openDocuments_.insert(absolute, content);
        versions_.insert(absolute, 1);
        notify(QStringLiteral("textDocument/didOpen"), {{"textDocument", QJsonObject{{"uri", uri},
            {"languageId", language_}, {"version", 1}, {"text", content}}}});
    } else if (openDocuments_.value(absolute) != content) {
        openDocuments_.insert(absolute, content);
        const int version = versions_.value(absolute) + 1;
        versions_.insert(absolute, version);
        const QJsonObject change{{"text", content}};
        const QJsonObject params{{"textDocument", QJsonObject{{"uri", uri}, {"version", version}}},
            {"contentChanges", QJsonArray{change}}};
        notify(QStringLiteral("textDocument/didChange"), params);
    }
}

QJsonObject LanguageService::documentParams(const QString &path) const
{
    return {{"textDocument", QJsonObject{{"uri", uriFor(path)}}}};
}

QJsonObject LanguageService::positionParams(const QJsonObject &args) const
{
    QJsonObject params = documentParams(args.value("filePath").toString());
    params.insert("position", pos(args.value("line").toInt(), args.value("character").toInt()));
    return params;
}

QJsonObject LanguageService::location(const QJsonObject &value)
{
    const QString uri = value.value("uri").toString(value.value("targetUri").toString());
    const QJsonObject range = value.value("range").toObject(value.value("targetRange").toObject());
    const QJsonObject start = range.value("start").toObject();
    const QJsonObject end = range.value("end").toObject();
    return {{"filePath", pathFor(uri)}, {"startLine", start.value("line")}, {"startChar", start.value("character")},
        {"endLine", end.value("line")}, {"endChar", end.value("character")}};
}

QJsonArray LanguageService::locations(const QJsonValue &value)
{
    QJsonArray output;
    const QJsonArray input = value.isArray() ? value.toArray() : (value.isObject() ? QJsonArray{value} : QJsonArray{});
    for (const auto &item : input) output.append(location(item.toObject()));
    return output;
}

QJsonArray LanguageService::codeActions(const QJsonObject &args)
{
    QJsonObject params = documentParams(args.value("filePath").toString());
    params.insert("range", QJsonObject{{"start", pos(args.value("startLine").toInt(), args.value("startChar").toInt())},
        {"end", pos(args.value("endLine").toInt(), args.value("endChar").toInt())}});
    params.insert("context", QJsonObject{{"diagnostics", QJsonArray{}}});
    return request(QStringLiteral("textDocument/codeAction"), params).toArray();
}

QJsonObject LanguageService::applyWorkspaceEdit(const QJsonObject &edit)
{
    QHash<QString, QJsonArray> editsByFile;
    const QJsonObject changes = edit.value("changes").toObject();
    for (auto it = changes.begin(); it != changes.end(); ++it) editsByFile.insert(pathFor(it.key()), it.value().toArray());
    for (const auto &entry : edit.value("documentChanges").toArray()) {
        const QJsonObject item = entry.toObject();
        if (item.contains("edits")) editsByFile.insert(pathFor(item.value("textDocument").toObject().value("uri").toString()), item.value("edits").toArray());
    }
    int filesChanged = 0;
    int editsApplied = 0;
    for (auto it = editsByFile.begin(); it != editsByFile.end(); ++it) {
        const QString path = QFileInfo(it.key()).absoluteFilePath();
        const QString root = QDir(workspace_).canonicalPath();
        if (path.compare(root, Qt::CaseInsensitive) != 0 && !path.startsWith(root + '/', Qt::CaseInsensitive))
            throw QStringLiteral("语言服务尝试修改工作区外文件：") + path;
        const QString canonical = QFileInfo(path).canonicalFilePath();
        if (!canonical.isEmpty() && canonical.compare(root, Qt::CaseInsensitive) != 0 &&
            !canonical.startsWith(root + '/', Qt::CaseInsensitive))
            throw QStringLiteral("语言服务尝试通过链接修改工作区外文件：") + path;
        QFile source(path);
        if (!source.open(QIODevice::ReadOnly)) throw QStringLiteral("无法读取待编辑文件：") + path;
        QString content = QString::fromUtf8(source.readAll());
        source.close();
        struct Edit { int start; int end; QString text; };
        QList<Edit> converted;
        for (const auto &value : it.value()) {
            const QJsonObject item = value.toObject();
            const QJsonObject range = item.value("range").toObject();
            converted.append({offsetAt(content, range.value("start").toObject()),
                offsetAt(content, range.value("end").toObject()), item.value("newText").toString()});
        }
        std::sort(converted.begin(), converted.end(), [](const Edit &a, const Edit &b) { return a.start > b.start; });
        for (const Edit &item : converted) {
            if (item.end < item.start) throw QStringLiteral("语言服务返回了无效文本范围");
            content.replace(item.start, item.end - item.start, item.text);
            ++editsApplied;
        }
        QSaveFile target(path);
        if (!target.open(QIODevice::WriteOnly)) throw QStringLiteral("无法写入语言服务编辑：") + path;
        const QByteArray bytes = content.toUtf8();
        if (target.write(bytes) != bytes.size() || !target.commit()) throw QStringLiteral("保存语言服务编辑失败：") + path;
        ++filesChanged;
        if (openDocuments_.contains(path)) {
            openDocuments_.insert(path, content);
            const int version = versions_.value(path) + 1;
            versions_.insert(path, version);
            notify(QStringLiteral("textDocument/didChange"),
                {{"textDocument", QJsonObject{{"uri", uriFor(path)}, {"version", version}}},
                 {"contentChanges", QJsonArray{QJsonObject{{"text", content}}}}});
        }
    }
    return {{"filesChanged", filesChanged}, {"editsApplied", editsApplied}};
}

QJsonValue LanguageService::workspaceSymbols(const QString &query)
{
    QJsonArray output;
    for (const auto &value : request(QStringLiteral("workspace/symbol"), {{"query", query}}).toArray()) {
        const QJsonObject item = value.toObject();
        const QJsonObject loc = item.value("location").toObject();
        const QJsonObject range = loc.value("range").toObject();
        output.append(QJsonObject{{"name", item.value("name")}, {"kind", kindName(item.value("kind").toInt())},
            {"filePath", pathFor(loc.value("uri").toString())},
            {"startLine", range.value("start").toObject().value("line")},
            {"containerName", item.value("containerName")}});
    }
    return output;
}

QJsonValue LanguageService::call(const QString &tool, const QJsonObject &args, const QString &content)
{
    const QString path = QFileInfo(args.value("filePath").toString()).absoluteFilePath();
    prepareDocument(path, content);
    if (tool == QStringLiteral("get_diagnostics")) {
        QElapsedTimer wait; wait.start();
        while (!diagnostics_.contains(path) && wait.elapsed() < 900) {
            process_.waitForReadyRead(100); readMessages();
        }
        QJsonArray output;
        const QString minimum = args.value("severity").toString();
        const QHash<QString, int> levels{{"hint",1},{"information",2},{"warning",3},{"error",4}};
        for (const auto &value : diagnostics_.value(path)) {
            const QJsonObject item = value.toObject();
            const int severity = item.value("severity").toInt(3);
            const int score = 5 - severity;
            if (!minimum.isEmpty() && score < levels.value(minimum, 0)) continue;
            const QJsonObject range = item.value("range").toObject();
            const QJsonObject start = range.value("start").toObject();
            const QJsonObject end = range.value("end").toObject();
            const QStringList names{"error","warning","information","hint"};
            output.append(QJsonObject{{"filePath", path}, {"severity", names.value(severity-1, "information")},
                {"message", item.value("message")}, {"source", item.value("source")}, {"code", item.value("code")},
                {"startLine", start.value("line")}, {"startChar", start.value("character")},
                {"endLine", end.value("line")}, {"endChar", end.value("character")}});
        }
        return output;
    }
    if (tool == QStringLiteral("find_references")) {
        QJsonObject params = positionParams(args);
        params.insert("context", QJsonObject{{"includeDeclaration", args.value("includeDeclaration").toBool(true)}});
        return locations(request(QStringLiteral("textDocument/references"), params));
    }
    if (tool == QStringLiteral("go_to_definition")) return locations(request(QStringLiteral("textDocument/definition"), positionParams(args)));
    if (tool == QStringLiteral("get_hover")) {
        const QJsonObject hover = request(QStringLiteral("textDocument/hover"), positionParams(args)).toObject();
        const QJsonValue contents = hover.value("contents");
        QJsonArray lines;
        const auto add = [&lines](const QJsonValue &value) {
            lines.append(value.isString() ? value.toString() : value.toObject().value("value").toString());
        };
        if (contents.isArray()) for (const auto &item : contents.toArray()) add(item);
        else if (!contents.isUndefined() && !contents.isNull()) add(contents);
        return QJsonObject{{"contents", lines}};
    }
    if (tool == QStringLiteral("get_document_symbols")) {
        QJsonArray output;
        for (const auto &value : request(QStringLiteral("textDocument/documentSymbol"), documentParams(path)).toArray())
            output.append(symbol(value.toObject()));
        return output;
    }
    if (tool == QStringLiteral("get_code_actions") || tool == QStringLiteral("apply_code_action")) {
        const QJsonArray actions = codeActions(args);
        if (tool == QStringLiteral("get_code_actions")) {
            QJsonArray output;
            for (int i = 0; i < actions.size(); ++i) {
                const QJsonObject item = actions[i].toObject();
                output.append(QJsonObject{{"index",i},{"title",item.value("title")},{"kind",item.value("kind")},
                    {"isPreferred",item.value("isPreferred").toBool(false)}});
            }
            return output;
        }
        const int index = args.value("actionIndex").toInt(-1);
        if (index < 0 || index >= actions.size()) throw QStringLiteral("代码操作索引无效");
        const QJsonObject action = actions[index].toObject();
        bool applied = false;
        if (action.value("edit").isObject()) { applyWorkspaceEdit(action.value("edit").toObject()); applied = true; }
        const QJsonObject command = action.value("command").isObject() ? action.value("command").toObject() : action;
        if (command.value("command").isString()) {
            request(QStringLiteral("workspace/executeCommand"), {{"command",command.value("command")},
                {"arguments",command.value("arguments").toArray()}});
            applied = true;
        }
        return QJsonObject{{"applied",applied},{"title",action.value("title")}};
    }
    if (tool == QStringLiteral("rename_symbol")) {
        QJsonObject params = positionParams(args);
        params.insert("newName", args.value("newName"));
        const QJsonObject edit = request(QStringLiteral("textDocument/rename"), params).toObject();
        if (edit.isEmpty()) throw QStringLiteral("当前位置不支持重命名");
        return applyWorkspaceEdit(edit);
    }
    throw QStringLiteral("未实现的语言服务工具：") + tool;
}
