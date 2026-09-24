#include "editorbridge.h"

#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QDialog>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QVBoxLayout>

namespace {
QString languageFor(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == "cpp" || suffix == "cc" || suffix == "cxx") return "cpp";
    if (suffix == "h" || suffix == "hpp" || suffix == "hh") return "cpp";
    if (suffix == "py") return "python";
    if (suffix == "ts" || suffix == "tsx") return "typescript";
    if (suffix == "js" || suffix == "jsx") return "javascript";
    if (suffix == "rs") return "rust";
    if (suffix == "go") return "go";
    return suffix.isEmpty() ? QStringLiteral("plaintext") : suffix;
}
QJsonObject position(const QTextDocument *document, int offset)
{
    const QTextBlock block = document->findBlock(offset);
    return {{"line", block.blockNumber()}, {"character", offset - block.position()}};
}
}

EditorBridge::EditorBridge(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *bar = new QHBoxLayout();
    auto *heading = new QLabel(QStringLiteral("本机编辑器 · MCP 活动文件与选区"), this);
    auto *openButton = new QPushButton(QStringLiteral("打开文件"), this);
    auto *saveButton = new QPushButton(QStringLiteral("保存"), this);
    bar->addWidget(heading); bar->addStretch(); bar->addWidget(openButton); bar->addWidget(saveButton);
    tabs_ = new QTabWidget(this);
    tabs_->setTabsClosable(true);
    tabs_->setDocumentMode(true);
    tabs_->setMinimumHeight(240);
    layout->addLayout(bar); layout->addWidget(tabs_);
    connect(openButton, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("打开代码文件"));
        if (!path.isEmpty()) {
            try { openFile(path); }
            catch (const QString &error) { QMessageBox::warning(this, QStringLiteral("打开失败"), error); }
        }
    });
    connect(saveButton, &QPushButton::clicked, this, [this]() {
        if (auto *editor = activeEditor()) {
            try { saveEditor(editor); }
            catch (const QString &error) { QMessageBox::warning(this, QStringLiteral("保存失败"), error); }
        }
    });
    connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int index) {
        auto *editor = qobject_cast<QPlainTextEdit *>(tabs_->widget(index));
        if (!editor) return;
        try { invokeTool(QStringLiteral("close_file"), {{"filePath", editor->property("filePath").toString()}}); }
        catch (const QString &error) { QMessageBox::warning(this, QStringLiteral("关闭失败"), error); }
    });
}

void EditorBridge::setWorkspace(const QString &path) { workspace_ = QDir(path).absolutePath(); }

int EditorBridge::tabIndex(const QString &path) const
{
    const QString canonical = QFileInfo(path).absoluteFilePath();
    for (int i = 0; i < tabs_->count(); ++i)
        if (tabs_->widget(i)->property("filePath").toString().compare(canonical, Qt::CaseInsensitive) == 0) return i;
    return -1;
}

QPlainTextEdit *EditorBridge::editorForPath(const QString &path) const
{
    const int index = tabIndex(path);
    return index < 0 ? nullptr : qobject_cast<QPlainTextEdit *>(tabs_->widget(index));
}

QPlainTextEdit *EditorBridge::activeEditor() const
{
    return qobject_cast<QPlainTextEdit *>(tabs_->currentWidget());
}

QString EditorBridge::activePath() const
{
    auto *editor = activeEditor();
    return editor ? editor->property("filePath").toString() : QString{};
}

QString EditorBridge::textForPath(const QString &path) const
{
    auto *editor = editorForPath(path);
    return editor ? editor->toPlainText() : QString{};
}

QPlainTextEdit *EditorBridge::openFile(const QString &path, int line, int character)
{
    const QString absolute = QFileInfo(path).absoluteFilePath();
    QPlainTextEdit *editor = editorForPath(absolute);
    if (!editor) {
        QFile file(absolute);
        if (!file.open(QIODevice::ReadOnly)) throw QStringLiteral("无法读取文件：") + file.errorString();
        editor = new QPlainTextEdit(tabs_);
        editor->setProperty("filePath", absolute);
        editor->setLineWrapMode(QPlainTextEdit::NoWrap);
        editor->setTabStopDistance(editor->fontMetrics().horizontalAdvance(' ') * 4);
        editor->setPlainText(QString::fromUtf8(file.readAll()));
        editor->document()->setModified(false);
        const int index = tabs_->addTab(editor, QFileInfo(absolute).fileName());
        tabs_->setTabToolTip(index, absolute);
        connect(editor->document(), &QTextDocument::modificationChanged, this, [this, editor](bool dirty) {
            const int currentIndex = tabs_->indexOf(editor);
            if (currentIndex >= 0) tabs_->setTabText(currentIndex,
                QFileInfo(editor->property("filePath").toString()).fileName() + (dirty ? QStringLiteral(" *") : QString{}));
        });
    }
    tabs_->setCurrentWidget(editor);
    if (line >= 0) {
        const QTextBlock block = editor->document()->findBlockByNumber(line);
        if (block.isValid()) {
            QTextCursor cursor(block);
            cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, qMax(0, character));
            editor->setTextCursor(cursor);
            editor->centerCursor();
        }
    }
    return editor;
}

void EditorBridge::saveEditor(QPlainTextEdit *editor)
{
    const QString path = editor->property("filePath").toString();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) throw QStringLiteral("无法保存文件：") + file.errorString();
    const QByteArray bytes = editor->toPlainText().toUtf8();
    if (file.write(bytes) != bytes.size() || !file.commit()) throw QStringLiteral("保存文件失败：") + file.errorString();
    editor->document()->setModified(false);
}

void EditorBridge::refreshFile(const QString &path)
{
    auto *editor = editorForPath(path);
    if (!editor || editor->document()->isModified()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return;
    const QSignalBlocker blocker(editor->document());
    editor->setPlainText(QString::fromUtf8(file.readAll()));
    editor->document()->setModified(false);
}

QJsonValue EditorBridge::invokeTool(const QString &name, const QJsonObject &args)
{
    const QString path = args.value(QStringLiteral("filePath")).toString();
    if (name == QStringLiteral("get_active_file")) {
        auto *editor = activeEditor();
        if (!editor) return QJsonValue::Null;
        const QString current = activePath();
        return QJsonObject{{"path", current}, {"relativePath", workspace_.isEmpty() ? QFileInfo(current).fileName() : QDir(workspace_).relativeFilePath(current)},
            {"content", editor->toPlainText()}, {"language", languageFor(current)},
            {"isDirty", editor->document()->isModified()}, {"lineCount", editor->document()->blockCount()}};
    }
    if (name == QStringLiteral("get_selection")) {
        auto *editor = activeEditor();
        if (!editor) return QJsonValue::Null;
        const QTextCursor cursor = editor->textCursor();
        const QJsonObject start = position(editor->document(), cursor.selectionStart());
        const QJsonObject end = position(editor->document(), cursor.selectionEnd());
        return QJsonObject{{"text", cursor.selectedText().replace(QChar::ParagraphSeparator, '\n')},
            {"startLine", start.value("line")}, {"startChar", start.value("character")},
            {"endLine", end.value("line")}, {"endChar", end.value("character")},
            {"isEmpty", !cursor.hasSelection()}, {"filePath", activePath()}};
    }
    if (name == QStringLiteral("get_open_tabs")) {
        QJsonArray items;
        for (int i = 0; i < tabs_->count(); ++i) {
            auto *editor = qobject_cast<QPlainTextEdit *>(tabs_->widget(i));
            if (!editor) continue;
            const QString current = editor->property("filePath").toString();
            items.append(QJsonObject{{"path", current}, {"relativePath", workspace_.isEmpty() ? QFileInfo(current).fileName() : QDir(workspace_).relativeFilePath(current)},
                {"language", languageFor(current)}, {"isDirty", editor->document()->isModified()},
                {"isActive", i == tabs_->currentIndex()}, {"type", "file"}});
        }
        return items;
    }
    if (name == QStringLiteral("open_file")) {
        openFile(path, args.value("line").toInt(-1), args.value("character").toInt());
        return QJsonObject{{"opened", true}, {"filePath", QFileInfo(path).absoluteFilePath()}};
    }
    if (name == QStringLiteral("close_file")) {
        const int index = tabIndex(path);
        if (index < 0) return QJsonObject{{"closed", 0}, {"filePath", path}};
        auto *editor = qobject_cast<QPlainTextEdit *>(tabs_->widget(index));
        if (editor && editor->document()->isModified()) throw QStringLiteral("文件有未保存更改，请先在本机编辑器中保存：") + path;
        tabs_->removeTab(index);
        editor->deleteLater();
        return QJsonObject{{"closed", 1}, {"filePath", path}};
    }
    if (name == QStringLiteral("show_diff")) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) throw QStringLiteral("无法读取差异原文件：") + file.errorString();
        auto *dialog = new QDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(args.value("title").toString(QStringLiteral("建议更改：") + QFileInfo(path).fileName()));
        dialog->resize(1100, 650);
        auto *layout = new QHBoxLayout(dialog);
        auto *oldView = new QPlainTextEdit(dialog); oldView->setReadOnly(true); oldView->setPlainText(QString::fromUtf8(file.readAll()));
        auto *newView = new QPlainTextEdit(dialog); newView->setReadOnly(true); newView->setPlainText(args.value("newContent").toString());
        layout->addWidget(oldView); layout->addWidget(newView);
        dialog->show();
        return QJsonObject{{"shown", true}, {"filePath", path}};
    }
    if (name == QStringLiteral("save_active_file")) {
        auto *editor = activeEditor();
        if (!editor) throw QStringLiteral("没有活动文件");
        saveEditor(editor);
        return QJsonObject{{"saved", true}, {"filePath", activePath()}};
    }
    throw QStringLiteral("不支持的编辑器操作：") + name;
}
