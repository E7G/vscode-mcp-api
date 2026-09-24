#pragma once

#include <QObject>
#include <QProcess>
#include <QNetworkAccessManager>
#include <QNetworkReply>

class NgrokManager final : public QObject
{
    Q_OBJECT
public:
    explicit NgrokManager(QObject *parent = nullptr);
    bool isRunning() const;
    QString publicUrl() const;
    QString executable() const;
    void setExecutable(const QString &path);
    void start(int port, const QString &token, const QString &domain = {});
    void stop();

signals:
    void statusChanged(bool running, const QString &url);
    void logMessage(const QString &message);
    void errorOccurred(const QString &message);

private slots:
    void consumeOutput();
    void pollInspector();

private:
    void emitStatus();
    QProcess process_;
    QNetworkAccessManager network_;
    QString executable_;
    QString publicUrl_;
    QString configPath_;
    QString activeToken_;
    QByteArray outputBuffer_;
    int port_ = 0;
    bool userStopped_ = false;
    bool installing_ = false;
};
