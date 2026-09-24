#include "ngrokmanager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryFile>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

namespace {
const QUrl DownloadUrl(QStringLiteral("https://bin.ngrok.com/c/bNyj1mQVY4c/ngrok-v3-stable-windows-amd64.zip"));
}

NgrokManager::NgrokManager(QObject *parent) : QObject(parent)
{
    process_.setProcessChannelMode(QProcess::MergedChannels);
    connect(&process_, &QProcess::readyRead, this, &NgrokManager::consumeOutput);
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (!userStopped_) emit errorOccurred(QStringLiteral("ngrok 启动失败：%1").arg(process_.errorString()));
        emitStatus();
    });
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) {
        consumeOutput();
        if (!configPath_.isEmpty()) { QFile::remove(configPath_); configPath_.clear(); }
        activeToken_.clear();
        if (!userStopped_ && code != 0)
            emit errorOccurred(QStringLiteral("ngrok 已退出，代码 %1。检查 authtoken 与网络连接。").arg(code));
        publicUrl_.clear();
        emitStatus();
    });
    auto *timer = new QTimer(this);
    timer->setInterval(1500);
    connect(timer, &QTimer::timeout, this, &NgrokManager::pollInspector);
    timer->start();
}

bool NgrokManager::isRunning() const { return process_.state() != QProcess::NotRunning || installing_; }
QString NgrokManager::publicUrl() const { return publicUrl_; }
QString NgrokManager::executable() const { return executable_; }
void NgrokManager::setExecutable(const QString &path) { executable_ = path; }

void NgrokManager::start(int port, const QString &token, const QString &domain)
{
    if (isRunning()) return;
    port_ = port;
    userStopped_ = false;
    publicUrl_.clear();
    QString binary = executable_;
    if (binary.isEmpty()) binary = QStandardPaths::findExecutable(QStringLiteral("ngrok"));
#ifdef Q_OS_WIN
    if (binary.isEmpty()) {
        const QString bundled = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                                    .filePath(QStringLiteral("bin/ngrok.exe"));
        if (QFile::exists(bundled)) binary = bundled;
        else {
            if (token.trimmed().isEmpty()) {
                emit errorOccurred(QStringLiteral("请先输入 ngrok authtoken（可在 ngrok.com 注册免费账号获取）"));
                return;
            }
            installing_ = true;
            emitStatus();
            emit logMessage(QStringLiteral("首次启动：正在从 ngrok 官方下载站获取 Windows Agent…"));
            auto *reply = network_.get(QNetworkRequest(DownloadUrl));
            connect(reply, &QNetworkReply::finished, this, [this, reply, token, domain]() {
                installing_ = false;
                const QByteArray archive = reply->readAll();
                if (reply->error() != QNetworkReply::NoError || archive.isEmpty()) {
                    emit errorOccurred(QStringLiteral("ngrok 下载失败：%1").arg(reply->errorString()));
                    reply->deleteLater(); emitStatus(); return;
                }
                const QString binDir = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                                           .filePath(QStringLiteral("bin"));
                QDir().mkpath(binDir);
                const QString zipPath = QDir(binDir).filePath(QStringLiteral("ngrok.zip"));
                QFile zip(zipPath);
                if (!zip.open(QIODevice::WriteOnly) || zip.write(archive) != archive.size()) {
                    emit errorOccurred(QStringLiteral("无法保存 ngrok 安装包"));
                    reply->deleteLater(); emitStatus(); return;
                }
                zip.close(); reply->deleteLater();
                QProcess unzip;
                unzip.start(QStringLiteral("tar"), {QStringLiteral("-xf"), zipPath,
                                                      QStringLiteral("-C"), binDir});
                if (!unzip.waitForStarted() || !unzip.waitForFinished(30000) || unzip.exitCode() != 0) {
                    emit errorOccurred(QStringLiteral("自动解压 ngrok 失败。安装 Windows tar 或从 ngrok.com/download/windows 安装。"));
                    emitStatus(); return;
                }
                QFile::remove(zipPath);
                executable_ = QDir(binDir).filePath(QStringLiteral("ngrok.exe"));
                if (!QFile::exists(executable_)) {
                    emit errorOccurred(QStringLiteral("ngrok 下载包中未找到 ngrok.exe")); emitStatus(); return;
                }
                start(port_, token, domain);
            });
            return;
        }
    }
#endif
    if (binary.isEmpty()) {
        emit errorOccurred(QStringLiteral("找不到 ngrok。将 ngrok.exe 放入 PATH，或设置其完整路径。"));
        return;
    }
    executable_ = binary;
    QStringList args{QStringLiteral("http"), QString::number(port), QStringLiteral("--log=stdout"),
                     QStringLiteral("--log-format=json")};
    const QString tokenValue = token.trimmed();
    if (tokenValue.isEmpty()) {
        emit errorOccurred(QStringLiteral("请先填写 ngrok authtoken。"));
        return;
    }
    QTemporaryFile config(QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                              .filePath(QStringLiteral("vscode-mcp-ngrok-XXXXXX.yml")));
    config.setAutoRemove(false);
    if (!config.open()) {
        emit errorOccurred(QStringLiteral("无法创建 ngrok 临时配置文件")); return;
    }
    configPath_ = config.fileName();
    QByteArray yamlToken = tokenValue.toUtf8();
    yamlToken.replace("'", "''");
    const QByteArray configBytes = "version: 3\nagent:\n  authtoken: '" + yamlToken + "'\n";
    if (config.write(configBytes) != configBytes.size() || !config.flush()) {
        config.close(); QFile::remove(configPath_); configPath_.clear();
        emit errorOccurred(QStringLiteral("无法写入 ngrok 临时配置文件")); return;
    }
    config.close();
    args << QStringLiteral("--config") << configPath_;
    if (!domain.trimmed().isEmpty()) args << QStringLiteral("--url") << domain.trimmed();
    activeToken_ = tokenValue;
    process_.setProcessEnvironment(QProcessEnvironment::systemEnvironment());
    process_.start(binary, args);
    QTimer::singleShot(3000, this, [this, configPath = configPath_]() {
        QFile::remove(configPath);
        if (configPath_ == configPath) configPath_.clear();
    });
    emitStatus();
}

void NgrokManager::stop()
{
    userStopped_ = true;
    if (process_.state() != QProcess::NotRunning) {
        process_.terminate();
        if (!process_.waitForFinished(1200)) process_.kill();
    }
    if (!configPath_.isEmpty()) { QFile::remove(configPath_); configPath_.clear(); }
    activeToken_.clear();
    publicUrl_.clear();
    emitStatus();
}

void NgrokManager::consumeOutput()
{
    outputBuffer_.append(process_.readAll());
    while (true) {
        const qsizetype end = outputBuffer_.indexOf('\n');
        if (end < 0) break;
        QByteArray line = outputBuffer_.left(end).trimmed();
        outputBuffer_.remove(0, end + 1);
        if (line.isEmpty()) continue;
        const QByteArray secret = activeToken_.toUtf8();
        if (!secret.isEmpty()) line.replace(secret, QByteArrayLiteral("[已隐藏]"));
        const auto doc = QJsonDocument::fromJson(line);
        if (doc.isObject()) {
            const auto obj = doc.object();
            const QString candidate = obj.value(QStringLiteral("url")).toString();
            if (candidate.startsWith(QStringLiteral("https://"))) {
                publicUrl_ = candidate; emitStatus();
            }
            const QString msg = obj.value(QStringLiteral("msg")).toString();
            if (!msg.isEmpty()) emit logMessage(QStringLiteral("ngrok: ") + msg);
        } else {
            emit logMessage(QStringLiteral("ngrok: ") + QString::fromUtf8(line));
        }
    }
    if (outputBuffer_.size() > 64 * 1024) outputBuffer_.clear();
}

void NgrokManager::pollInspector()
{
    if (process_.state() == QProcess::NotRunning || network_.property("polling").toBool()) return;
    network_.setProperty("polling", true);
    auto *reply = network_.get(QNetworkRequest(QUrl(QStringLiteral("http://127.0.0.1:4040/api/tunnels"))));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        network_.setProperty("polling", false);
        if (reply->error() == QNetworkReply::NoError) {
            const auto doc = QJsonDocument::fromJson(reply->readAll());
            const auto tunnels = doc.object().value(QStringLiteral("tunnels")).toArray();
            for (const auto &entry : tunnels) {
                const auto obj = entry.toObject();
                const QString candidate = obj.value(QStringLiteral("public_url")).toString();
                if (candidate.startsWith(QStringLiteral("https://"))) {
                    if (candidate != publicUrl_) { publicUrl_ = candidate; emitStatus(); }
                    break;
                }
            }
        }
        reply->deleteLater();
    });
}

void NgrokManager::emitStatus() { emit statusChanged(isRunning(), publicUrl_); }
