#include "documenttools.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>
#include <QStandardPaths>
#include <QXmlStreamReader>

namespace {
QByteArray processOutput(const QString &exe, const QStringList &args, bool optional = false)
{
    QProcess process;
    process.start(exe, args);
    if (!process.waitForStarted(3000)) {
        if (optional) return {};
        throw QStringLiteral("无法启动文档解析后端：") + exe;
    }
    QByteArray bytes;
    QElapsedTimer timer;
    timer.start();
    while (process.state() != QProcess::NotRunning && timer.elapsed() < 15000) {
        process.waitForReadyRead(100);
        bytes += process.readAllStandardOutput();
        if (bytes.size() > 4 * 1024 * 1024) {
            process.kill(); process.waitForFinished(1000);
            throw QStringLiteral("文档解压结果超过 4 MiB 限制");
        }
    }
    bytes += process.readAllStandardOutput();
    if (process.state() != QProcess::NotRunning) {
        process.kill(); process.waitForFinished(1000);
        throw QStringLiteral("文档解析超时");
    }
    if (bytes.size() > 4 * 1024 * 1024) throw QStringLiteral("文档解压结果过大");
    if (process.exitCode() != 0) {
        if (optional) return {};
        throw QStringLiteral("文档内容无法解析：") + QString::fromLocal8Bit(process.readAllStandardError()).left(300);
    }
    return bytes;
}

QByteArray zipEntry(const QString &archive, const QString &entry, bool optional = false)
{
#ifdef Q_OS_WIN
    return processOutput(QStringLiteral("tar.exe"),
        {QStringLiteral("-xOf"), archive, entry}, optional);
#else
    return processOutput(QStringLiteral("unzip"),
        {QStringLiteral("-p"), archive, entry}, optional);
#endif
}

QString readWord(const QByteArray &xml, int maxChars, bool &truncated)
{
    QXmlStreamReader reader(xml);
    QString text;
    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token == QXmlStreamReader::StartElement) {
            if (reader.name() == QStringLiteral("t")) text += reader.readElementText();
            else if (reader.name() == QStringLiteral("tab")) text += '\t';
            else if (reader.name() == QStringLiteral("br")) text += '\n';
        } else if (token == QXmlStreamReader::EndElement && reader.name() == QStringLiteral("p")) {
            text += '\n';
        }
        if (text.size() > maxChars) { text.truncate(maxChars); truncated = true; break; }
    }
    if (reader.hasError() && !truncated)
        throw QStringLiteral("Word 文档 XML 格式错误：") + reader.errorString();
    return text;
}

QStringList sharedStrings(const QByteArray &xml)
{
    QStringList values;
    if (xml.isEmpty()) return values;
    QXmlStreamReader reader(xml);
    QString item;
    bool inItem = false;
    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token == QXmlStreamReader::StartElement) {
            if (reader.name() == QStringLiteral("si")) { item.clear(); inItem = true; }
            else if (inItem && reader.name() == QStringLiteral("t")) item += reader.readElementText();
        } else if (token == QXmlStreamReader::EndElement && reader.name() == QStringLiteral("si")) {
            values.append(item);
            inItem = false;
        }
        if (values.size() > 20000) throw QStringLiteral("共享字符串数量超过限制");
    }
    if (reader.hasError()) throw QStringLiteral("Excel 共享字符串解析失败");
    return values;
}

QJsonArray firstSheet(const QByteArray &xml, const QStringList &strings,
                      int limit, bool &truncated)
{
    QXmlStreamReader reader(xml);
    QJsonArray cells;
    QString address, type, value;
    bool inCell = false;
    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token == QXmlStreamReader::StartElement) {
            if (reader.name() == QStringLiteral("c")) {
                address = reader.attributes().value(QStringLiteral("r")).toString();
                type = reader.attributes().value(QStringLiteral("t")).toString();
                value.clear();
                inCell = true;
            } else if (inCell && (reader.name() == QStringLiteral("v") ||
                                  reader.name() == QStringLiteral("t"))) {
                value += reader.readElementText();
            }
        } else if (token == QXmlStreamReader::EndElement && reader.name() == QStringLiteral("c")) {
            if (type == QStringLiteral("s")) {
                bool ok = false;
                const int index = value.toInt(&ok);
                value = ok && index >= 0 && index < strings.size() ? strings.at(index) : QString{};
            }
            cells.append(QJsonObject{{"cell", address}, {"value", value}});
            if (cells.size() >= limit) { truncated = true; break; }
            inCell = false;
        }
    }
    if (reader.hasError() && !truncated)
        throw QStringLiteral("Excel 工作表 XML 格式错误：") + reader.errorString();
    return cells;
}
}

QJsonValue DocumentTools::read(const QJsonObject &args, const PathGuard &guard)
{
    const QString path = guard(args.value("filePath").toString(), true);
    const QFileInfo file(path);
    if (!file.isFile()) throw QStringLiteral("文档路径不是文件");
    if (file.size() > 40 * 1024 * 1024) throw QStringLiteral("文档超过 40 MiB 安全上限");
    const QString ext = file.suffix().toLower();
    bool truncated = false;
    if (ext == QStringLiteral("docx")) {
        const QByteArray xml = zipEntry(path, QStringLiteral("word/document.xml"));
        const int maxChars = qBound(1000, args.value("maxChars").toInt(100000), 500000);
        return QJsonObject{{"format", "docx"}, {"path", path},
                           {"text", readWord(xml, maxChars, truncated)},
                           {"truncated", truncated}, {"mode", "text-extraction"}};
    }
    if (ext == QStringLiteral("xlsx")) {
        const QStringList strings = sharedStrings(zipEntry(
            path, QStringLiteral("xl/sharedStrings.xml"), true));
        const QByteArray xml = zipEntry(path, QStringLiteral("xl/worksheets/sheet1.xml"));
        const int maxCells = qBound(1, args.value("maxCells").toInt(1000), 5000);
        return QJsonObject{{"format", "xlsx"}, {"path", path},
                           {"cells", firstSheet(xml, strings, maxCells, truncated)},
                           {"truncated", truncated}, {"sheet", 1},
                           {"mode", "first-sheet-extraction"}};
    }
    if (ext == QStringLiteral("pdf")) {
        const QString binary = QStandardPaths::findExecutable(QStringLiteral("pdftotext"));
        if (binary.isEmpty()) throw QStringLiteral("PDF 读取需要安装 Poppler 的 pdftotext 并加入 PATH");
        const QByteArray bytes = processOutput(binary, {QStringLiteral("-layout"), path, QStringLiteral("-")});
        const int maxChars = qBound(1000, args.value("maxChars").toInt(100000), 500000);
        QString text = QString::fromUtf8(bytes);
        if (text.size() > maxChars) { text.truncate(maxChars); truncated = true; }
        return QJsonObject{{"format", "pdf"}, {"path", path}, {"text", text},
                           {"truncated", truncated}, {"mode", "text-extraction"}};
    }
    throw QStringLiteral("read_document 支持 .docx、.xlsx 和 .pdf；老式二进制格式不支持");
}
