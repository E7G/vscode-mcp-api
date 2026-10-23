#include "workspacetools.h"

#include <QDateTime>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringConverter>
#include <QStringList>
#include <algorithm>

namespace {
QString stringArg(const QJsonObject &args, const char *key)
{
    return args.value(QLatin1String(key)).toString();
}

QJsonObject fileEntry(const QFileInfo &info, const QString &workspace)
{
    return {{"name", info.fileName()},
            {"path", info.absoluteFilePath()},
            {"relativePath", QDir(workspace).relativeFilePath(info.absoluteFilePath())},
            {"type", info.isDir() ? "directory" : (info.isFile() ? "file" : "other")},
            {"size", static_cast<qint64>(info.size())},
            {"symlink", info.isSymbolicLink()},
            {"modified", info.lastModified().toString(Qt::ISODateWithMs)}};
}

QByteArray readBounded(const QString &path, qsizetype maxBytes)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw QStringLiteral("无法读取文件：") + file.errorString();
    if (file.size() > maxBytes) throw QStringLiteral("文件超过安全读取上限：") + path;
    const QByteArray bytes = file.read(maxBytes + 1);
    if (bytes.size() > maxBytes) throw QStringLiteral("文件超过安全读取上限：") + path;
    return bytes;
}

QString decodeUtf8(const QByteArray &bytes)
{
    if (bytes.contains('\0')) throw QStringLiteral("不支持编辑二进制文件");
    QStringDecoder decoder(QStringConverter::Utf8);
    const QString value = decoder.decode(bytes);
    if (decoder.hasError()) throw QStringLiteral("文件不是有效 UTF-8，请使用对应格式工具");
    return value;
}

void saveUtf8(const QString &path, const QString &text)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) throw QStringLiteral("无法打开文件：") + file.errorString();
    const QByteArray bytes = text.toUtf8();
    if (file.write(bytes) != bytes.size() || !file.commit())
        throw QStringLiteral("原子写入失败：") + file.errorString();
}
}

bool WorkspaceTools::supports(const QString &name)
{
    static const QStringList names = {
        "list_directory", "get_file_info", "read_multiple_files",
        "create_directory", "move_file", "edit_block", "search_workspace"
    };
    return names.contains(name);
}

QJsonValue WorkspaceTools::call(const QString &name, const QJsonObject &args,
                                const QString &workspace, const PathGuard &guard)
{
    if (name == QStringLiteral("get_file_info")) {
        const QString path = guard(stringArg(args, "path"), true);
        const QFileInfo info(path);
        QJsonObject data = fileEntry(info, workspace);
        data.insert("created", info.birthTime().toString(Qt::ISODateWithMs));
        data.insert("readable", info.isReadable());
        data.insert("writable", info.isWritable());
        if (info.isFile() && info.size() <= 2 * 1024 * 1024) {
            const QByteArray bytes = readBounded(path, 2 * 1024 * 1024);
            if (!bytes.contains('\0'))
                data.insert("lineCount", static_cast<int>(std::count(bytes.begin(), bytes.end(), '\n')) + 1);
        }
        return data;
    }
    if (name == QStringLiteral("list_directory")) {
        const QString path = guard(stringArg(args, "path").isEmpty() ? "." : stringArg(args, "path"), true);
        if (!QFileInfo(path).isDir()) throw QStringLiteral("路径不是目录：") + path;
        const int depth = qBound(1, args.value("depth").toInt(1), 5);
        const int limit = qBound(1, args.value("maxEntries").toInt(500), 1500);
        QJsonArray entries;
        bool truncated = false;
        std::function<void(const QString &, int)> visit = [&](const QString &dir, int level) {
            if (entries.size() >= limit) { truncated = true; return; }
            const QFileInfoList children = QDir(dir).entryInfoList(
                QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot | QDir::System,
                QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
            for (const QFileInfo &info : children) {
                if (entries.size() >= limit) { truncated = true; return; }
                entries.append(fileEntry(info, workspace));
                if (info.isDir() && !info.isSymbolicLink() && level < depth) {
                    try { visit(guard(info.absoluteFilePath(), true), level + 1); }
                    catch (const QString &) { /* Do not follow links escaping the workspace. */ }
                }
            }
        };
        visit(path, 1);
        return QJsonObject{{"root", path}, {"entries", entries},
                           {"truncated", truncated}, {"depth", depth}};
    }
    if (name == QStringLiteral("read_multiple_files")) {
        const QJsonArray paths = args.value("paths").toArray();
        if (paths.isEmpty() || paths.size() > 20)
            throw QStringLiteral("paths 必须包含 1 到 20 个文件");
        QJsonArray files;
        qsizetype totalBytes = 0;
        for (const QJsonValue &entry : paths) {
            if (!entry.isString()) throw QStringLiteral("paths 只能包含字符串路径");
            const QString path = guard(entry.toString(), true);
            if (!QFileInfo(path).isFile()) throw QStringLiteral("不是常规文件：") + path;
            const QByteArray bytes = readBounded(path, 2 * 1024 * 1024);
            totalBytes += bytes.size();
            if (totalBytes > 8 * 1024 * 1024) throw QStringLiteral("批量读取总大小超过 8 MiB");
            files.append(QJsonObject{{"path", path}, {"content", decodeUtf8(bytes)},
                                     {"bytes", static_cast<qint64>(bytes.size())}});
        }
        return QJsonObject{{"files", files}, {"count", files.size()}};
    }
    if (name == QStringLiteral("create_directory")) {
        const QString path = guard(stringArg(args, "path"), false);
        if (!QDir().mkpath(path)) throw QStringLiteral("创建目录失败：") + path;
        // Re-check after creation in case an ancestor changed during the operation.
        guard(path, true);
        return QJsonObject{{"path", path}, {"created", true}};
    }
    if (name == QStringLiteral("move_file")) {
        const QString source = guard(stringArg(args, "source"), true);
        const QString destination = guard(stringArg(args, "destination"), false);
        if (source == destination) throw QStringLiteral("源路径与目标路径相同");
        if (QFileInfo::exists(destination)) throw QStringLiteral("目标已存在，不会覆盖：") + destination;
        if (!QDir().mkpath(QFileInfo(destination).absolutePath()))
            throw QStringLiteral("目标父目录无法创建");
        guard(destination, false);
        const QFileInfo sourceInfo(source);
        const bool moved = sourceInfo.isDir() ? QDir().rename(source, destination)
                                             : QFile::rename(source, destination);
        if (!moved) throw QStringLiteral("移动失败（可能跨卷或文件被占用）");
        return QJsonObject{{"source", source}, {"destination", destination}, {"moved", true}};
    }
    if (name == QStringLiteral("edit_block")) {
        const QString path = guard(stringArg(args, "filePath"), true);
        if (!QFileInfo(path).isFile()) throw QStringLiteral("只能编辑常规文件");
        const QString oldText = stringArg(args, "old_string");
        const QString newText = stringArg(args, "new_string");
        const int expected = args.value("expected_replacements").toInt(1);
        if (oldText.isEmpty() || expected < 1 || expected > 10000)
            throw QStringLiteral("old_string 不能为空；expected_replacements 须为正整数");
        const QByteArray originalBytes = readBounded(path, 8 * 1024 * 1024);
        const QString originalHash = QString::fromLatin1(QCryptographicHash::hash(originalBytes, QCryptographicHash::Sha256).toHex());
        const QString expectedHash = args.value("expectedSha256").toString().trimmed().toLower();
        if (args.contains("expectedSha256")) {
            static const QRegularExpression shaPattern(QStringLiteral("^[0-9a-f]{64}$"));
            if (!shaPattern.match(expectedHash).hasMatch()) throw QStringLiteral("expectedSha256 必须是 64 位十六进制 SHA-256");
            if (expectedHash != originalHash) throw QStringLiteral("版本冲突：文件 SHA-256 与 expectedSha256 不一致，未修改文件");
        }
        const QString content = decodeUtf8(originalBytes);
        const int matches = content.count(oldText, Qt::CaseSensitive);
        if (matches != expected)
            throw QStringLiteral("精确替换匹配数 %1，与预期 %2 不符；未修改文件").arg(matches).arg(expected);
        QString edited = content;
        edited.replace(oldText, newText, Qt::CaseSensitive);
        const QByteArray updatedBytes = edited.toUtf8();
        if (updatedBytes.size() > 8 * 1024 * 1024) throw QStringLiteral("编辑后的文件超过 8 MiB 大小限制");
        const QString updatedHash = QString::fromLatin1(QCryptographicHash::hash(updatedBytes, QCryptographicHash::Sha256).toHex());
        const bool dryRun = args.value("dryRun").toBool(false);
        if (dryRun)
            return QJsonObject{{"path", path}, {"dryRun", true}, {"replacements", matches},
                               {"oldSha256", originalHash}, {"newSha256", updatedHash},
                               {"bytesWritten", static_cast<qint64>(updatedBytes.size())}};
        guard(path, true);
        if (readBounded(path, 8 * 1024 * 1024) != originalBytes)
            throw QStringLiteral("写入前文件已被其他进程修改，已取消编辑");
        saveUtf8(path, edited);
        return QJsonObject{{"path", path}, {"dryRun", false}, {"replacements", matches},
                           {"oldSha256", originalHash}, {"newSha256", updatedHash},
                           {"bytesWritten", static_cast<qint64>(updatedBytes.size())}};
    }
    if (name == QStringLiteral("search_workspace")) {
        const QString root = guard(stringArg(args, "path").isEmpty() ? "." : stringArg(args, "path"), true);
        if (!QFileInfo(root).isDir()) throw QStringLiteral("搜索根目录不是目录");
        const QString query = stringArg(args, "pattern");
        if (query.isEmpty() || query.size() > 256) throw QStringLiteral("搜索内容长度应为 1–256 字符");
        const bool contentSearch = stringArg(args, "searchType") != QStringLiteral("files");
        const bool regex = args.value("regex").toBool(false);
        const bool ignoreCase = args.value("ignoreCase").toBool(true);
        const int maxResults = qBound(1, args.value("maxResults").toInt(100), 500);
        const int maxFiles = qBound(1, args.value("maxFiles").toInt(5000), 20000);
        const QString filePattern = stringArg(args, "filePattern");
        const QRegularExpression::PatternOptions options =
            ignoreCase ? QRegularExpression::CaseInsensitiveOption
                       : QRegularExpression::NoPatternOption;
        const QRegularExpression matcher(regex ? query : QRegularExpression::escape(query), options);
        if (!matcher.isValid()) throw QStringLiteral("无效正则表达式：") + matcher.errorString();
        const QRegularExpression glob(QRegularExpression::wildcardToRegularExpression(
            filePattern.isEmpty() ? "*" : filePattern), QRegularExpression::CaseInsensitiveOption);
        const QStringList excluded = {".git", "node_modules", ".cache", ".venv", "dist", "build"};
        QJsonArray matches;
        int scanned = 0;
        bool truncated = false;
        QElapsedTimer timer;
        timer.start();
        QDirIterator iterator(root, QDir::Files | QDir::NoSymLinks | QDir::Hidden,
                              QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            const QString path = iterator.next();
            const QString relative = QDir(root).relativeFilePath(path).replace('\\', '/');
            const QStringList segments = relative.split('/');
            bool excludedFile = false;
            for (const QString &segment : segments) {
                if (excluded.contains(segment, Qt::CaseInsensitive)) { excludedFile = true; break; }
            }
            if (excludedFile || !glob.match(QFileInfo(path).fileName()).hasMatch()) continue;
            if (++scanned > maxFiles || timer.elapsed() > 10000) { truncated = true; break; }
            try { guard(path, true); } catch (const QString &) { continue; }
            if (!contentSearch) {
                if (matcher.match(QFileInfo(path).fileName()).hasMatch())
                    matches.append(QJsonObject{{"path", path}, {"relativePath", relative}});
            } else {
                const QFileInfo info(path);
                if (info.size() > 1024 * 1024) continue;
                QFile file(path);
                if (!file.open(QIODevice::ReadOnly)) continue;
                const QByteArray bytes = file.read(1024 * 1024 + 1);
                if (bytes.contains('\0')) continue;
                const QStringList lines = QString::fromUtf8(bytes).split('\n');
                for (qsizetype i = 0; i < lines.size(); ++i) {
                    const auto match = matcher.match(lines.at(i));
                    if (!match.hasMatch()) continue;
                    matches.append(QJsonObject{{"path", path}, {"relativePath", relative},
                        {"line", static_cast<int>(i + 1)}, {"column", match.capturedStart() + 1},
                        {"preview", lines.at(i).trimmed().left(300)}});
                    if (matches.size() >= maxResults) break;
                }
            }
            if (matches.size() >= maxResults) { truncated = true; break; }
        }
        return QJsonObject{{"matches", matches}, {"scannedFiles", scanned}, {"truncated", truncated},
                           {"searchType", contentSearch ? "content" : "files"}};
    }
    throw QStringLiteral("不支持的文件工具：") + name;
}
