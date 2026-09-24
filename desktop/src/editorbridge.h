#pragma once

#include <QWidget>
#include <QJsonObject>
#include <QJsonValue>

class QTabWidget;
class QPlainTextEdit;

// Owns the native editor state used by MCP. All methods run on the GUI thread.
class EditorBridge final : public QWidget
{
    Q_OBJECT
public:
    explicit EditorBridge(QWidget *parent = nullptr);
    QJsonValue invokeTool(const QString &name, const QJsonObject &args);
    QString activePath() const;
    QString textForPath(const QString &path) const;
    void refreshFile(const QString &path);
    void setWorkspace(const QString &path);

private:
    QPlainTextEdit *activeEditor() const;
    QPlainTextEdit *editorForPath(const QString &path) const;
    QPlainTextEdit *openFile(const QString &path, int line = -1, int character = 0);
    void saveEditor(QPlainTextEdit *editor);
    int tabIndex(const QString &path) const;
    QTabWidget *tabs_ = nullptr;
    QString workspace_;
};
