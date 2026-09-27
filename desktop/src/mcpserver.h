#pragma once

#include <QObject>
#include <QTcpServer>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <QStringList>
#include <QJsonArray>
#include <atomic>

class QTcpSocket;
class QProcess;
class EditorBridge;
class LanguageService;

class McpServer final : public QObject
{
    Q_OBJECT
public:
    explicit McpServer(QObject *parent = nullptr);
    int port() const;
    int connectionCount() const;
    QString workspace() const;

public slots:
    void start(int port);
    void stop();
    void setWorkspace(const QString &path);
    void setRemoteTarget(const QString &sshHost, const QString &remoteRoot);
    void setAuthToken(const QString &token);
    void setEditorBridge(EditorBridge *editor);

signals:
    void serverStateChanged(bool running, int port, const QString &error);
    void logMessage(const QString &message);
    void clientCountChanged(int count);

private slots:
    void acceptConnections();
    void readRequest();

private:
    void handleRequest(QTcpSocket *socket, const QByteArray &request);
    void sendHttp(QTcpSocket *socket, int status, const QByteArray &body,
                  const QByteArray &contentType = "application/json");
    QJsonObject rpc(const QJsonObject &request);
    QJsonObject callTool(const QString &name, const QJsonObject &arguments, bool *isError);
    QJsonArray toolList() const;
    QJsonObject callEditor(const QString &name, const QJsonObject &args);
    QJsonObject callLanguage(const QString &name, const QJsonObject &args);
    QJsonObject schema(const QJsonObject &properties, const QStringList &required = {}) const;
    QString checkedPath(const QString &path, bool mustExist = false) const;
    QString checkedRemotePath(const QString &path) const;
    QJsonObject runRemote(const QString &command, const QByteArray &stdinData = {}, int timeoutMs = 30000) const;
    static QString shellQuote(const QString &value);
    QJsonObject result(const QJsonValue &value, bool isError = false) const;

    QTcpServer *tcpServer_ = nullptr;
    QString workspace_;
    QString sshHost_;
    QString remoteRoot_;
    QString authToken_;
    EditorBridge *editor_ = nullptr;
    QHash<QString, LanguageService *> languages_;
    QHash<QString, QProcess *> terminals_;
    int port_ = 0;
    std::atomic<int> connections_{0};
};
