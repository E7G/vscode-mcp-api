#pragma once

#include <QObject>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QString>

// Minimal native LSP 3.17 client. One lazy process per language/workspace.
class LanguageService final : public QObject
{
    Q_OBJECT
public:
    LanguageService(QString language, QString workspace, QObject *parent = nullptr);
    ~LanguageService() override;
    static QString languageFor(const QString &filePath);
    static bool available(const QString &language);
    QJsonValue call(const QString &tool, const QJsonObject &args, const QString &content);
    QJsonValue workspaceSymbols(const QString &query);

private:
    void start();
    void send(const QJsonObject &message);
    void notify(const QString &method, const QJsonObject &params);
    QJsonValue request(const QString &method, const QJsonObject &params, int timeoutMs = 30000);
    void readMessages();
    void prepareDocument(const QString &path, const QString &content);
    QJsonObject documentParams(const QString &path) const;
    QJsonObject positionParams(const QJsonObject &args) const;
    QJsonArray codeActions(const QJsonObject &args);
    QJsonObject applyWorkspaceEdit(const QJsonObject &edit);
    static QJsonObject location(const QJsonObject &value);
    static QJsonArray locations(const QJsonValue &value);

    QString language_;
    QString workspace_;
    QProcess process_;
    QByteArray buffer_;
    int nextId_ = 1;
    bool initialized_ = false;
    QHash<int, QJsonObject> replies_;
    QHash<QString, QString> openDocuments_;
    QHash<QString, int> versions_;
    QHash<QString, QJsonArray> diagnostics_;
};
