#include "mainwindow.h"
#include "credentials.h"
#include "mcpserver.h"
#include "ngrokmanager.h"
#include "editorbridge.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFrame>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QStyleHints>
#include <QSpinBox>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

namespace {
QWidget *card(QWidget *parent)
{
    auto *frame = new QFrame(parent);
    frame->setObjectName(QStringLiteral("card"));
    return frame;
}
QIcon trayIcon()
{
    QPixmap pixmap(64,64); pixmap.fill(Qt::transparent);
    QPainter p(&pixmap); p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(QColor("#1769e0")); p.setPen(Qt::NoPen); p.drawRoundedRect(3,3,58,58,15,15);
    p.setPen(QPen(Qt::white,4,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
    p.drawLine(20,20,20,44); p.drawLine(20,20,39,20); p.drawLine(20,32,35,32); p.drawLine(20,44,41,44);
    return QIcon(pixmap);
}
}

MainWindow::MainWindow(const QString &workspaceOverride, QWidget *parent) : QMainWindow(parent), workspaceOverride_(workspaceOverride)
{
    setWindowTitle(QStringLiteral("VS Code MCP Bridge"));
    setWindowIcon(trayIcon());
    resize(860, 760);
    loadSettings();
    buildUi();
    editorBridge_->setWorkspace(workspaceEdit_->text());
    buildTray();

    server_ = new McpServer();
    server_->setWorkspace(workspaceEdit_->text());
    server_->setAuthToken(authTokenEdit_->text());
    server_->setEditorBridge(editorBridge_);
    server_->moveToThread(&serverThread_);
    connect(&serverThread_, &QThread::finished, server_, &QObject::deleteLater);
    connect(server_, &McpServer::serverStateChanged, this, &MainWindow::updateServerState);
    connect(server_, &McpServer::logMessage, this, &MainWindow::appendLog);
    connect(server_, &McpServer::clientCountChanged, this, [this](int count) {
        if (serverRunning_) serverStatus_->setText(QStringLiteral("运行中 · %1 个连接").arg(count));
    });
    serverThread_.setObjectName(QStringLiteral("MCP HTTP server"));
    serverThread_.start();

    ngrok_ = new NgrokManager(this);
    ngrok_->setExecutable(ngrokPathEdit_->text());
    autoTunnelPending_ = !ngrokTokenEdit_->text().trimmed().isEmpty() &&
        !ngrokDomainEdit_->text().trimmed().isEmpty() &&
        !authTokenEdit_->text().trimmed().isEmpty();
    connect(ngrok_, &NgrokManager::statusChanged, this, &MainWindow::updateNgrokState);
    connect(ngrok_, &NgrokManager::logMessage, this, &MainWindow::appendLog);
    connect(ngrok_, &NgrokManager::errorOccurred, this, [this](const QString &error) {
        appendLog(error); QMessageBox::warning(this, QStringLiteral("ngrok"), error);
    });
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this](Qt::ColorScheme) {
        if (themeCombo_ && themeCombo_->currentData().toString() == QStringLiteral("auto")) applyTheme();
    });

    connect(workspaceEdit_, &QLineEdit::editingFinished, this, &MainWindow::saveSettings);
    connect(authTokenEdit_, &QLineEdit::editingFinished, this, &MainWindow::saveSettings);
    connect(portSpin_, &QSpinBox::valueChanged, this, &MainWindow::saveSettings);
    connect(ngrokDomainEdit_, &QLineEdit::editingFinished, this, &MainWindow::saveSettings);
    connect(ngrokPathEdit_, &QLineEdit::editingFinished, this, &MainWindow::saveSettings);
    QMetaObject::invokeMethod(server_, "start", Qt::QueuedConnection, Q_ARG(int, portSpin_->value()));
}

MainWindow::~MainWindow()
{
    if (ngrok_) ngrok_->stop();
    if (serverThread_.isRunning()) {
        QMetaObject::invokeMethod(server_, "stop", Qt::BlockingQueuedConnection);
        serverThread_.quit(); serverThread_.wait(3000);
    }
}

void MainWindow::buildUi()
{
    auto *root = new QWidget(this); root->setObjectName(QStringLiteral("root")); auto *layout = new QVBoxLayout(root);
    layout->setContentsMargins(28,22,28,20); layout->setSpacing(16);
    QSettings saved;
    auto *header=new QHBoxLayout();
    auto *title = new QLabel(QStringLiteral("VS Code MCP Bridge")); title->setObjectName("title");
    themeCombo_=new QComboBox(); themeCombo_->addItem(QStringLiteral("跟随系统"),QStringLiteral("auto"));
    themeCombo_->addItem(QStringLiteral("浅色"),QStringLiteral("light")); themeCombo_->addItem(QStringLiteral("深色"),QStringLiteral("dark"));
    const QString savedTheme=saved.value("theme",QStringLiteral("auto")).toString();
    themeCombo_->setCurrentIndex(qMax(0,themeCombo_->findData(savedTheme)));
    connect(themeCombo_,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int){saveSettings();applyTheme();});
    header->addWidget(title); header->addStretch(); header->addWidget(new QLabel(QStringLiteral("外观"))); header->addWidget(themeCombo_);
    auto *subtitle = new QLabel(QStringLiteral("轻量原生桌面服务 · 选择工作区后即可连接 MCP 客户端")); subtitle->setObjectName("muted");
    layout->addLayout(header); layout->addWidget(subtitle);

    auto *workspaceCard=card(root); auto *workspaceLayout=new QVBoxLayout(workspaceCard);
    auto *workspaceTitle=new QLabel(QStringLiteral("工作区")); workspaceTitle->setObjectName("sectionTitle");
    workspaceLayout->addWidget(workspaceTitle);
    auto *workspaceRow=new QHBoxLayout(); workspaceEdit_=new QLineEdit(workspaceOverride_.isEmpty() ? saved.value("workspace",QDir::homePath()).toString() : workspaceOverride_); workspaceEdit_->setPlaceholderText("选择 AI 工具可访问的项目目录");
    auto *browse=new QPushButton(QStringLiteral("浏览…")); connect(browse,&QPushButton::clicked,this,&MainWindow::chooseWorkspace);
    workspaceRow->addWidget(workspaceEdit_,1); workspaceRow->addWidget(browse); workspaceLayout->addLayout(workspaceRow);
    layout->addWidget(workspaceCard);

    auto *serverCard=card(root); auto *serverLayout=new QVBoxLayout(serverCard);
    auto *serverHead=new QHBoxLayout(); auto *serverTitle=new QLabel(QStringLiteral("MCP 本地服务")); serverTitle->setObjectName("sectionTitle");
    serverStatus_=new QLabel(QStringLiteral("启动中…")); serverStatus_->setObjectName("status"); serverHead->addWidget(serverTitle); serverHead->addStretch(); serverHead->addWidget(serverStatus_); serverLayout->addLayout(serverHead);
    auto *endpointRow=new QHBoxLayout(); localUrl_=new QLabel(QStringLiteral("http://127.0.0.1:%1/mcp").arg(saved.value("port",3333).toInt())); localUrl_->setTextInteractionFlags(Qt::TextSelectableByMouse); localUrl_->setObjectName("endpoint");
    auto *copyLocal=new QPushButton(QStringLiteral("复制地址")); connect(copyLocal,&QPushButton::clicked,this,&MainWindow::copyLocalUrl);
    serverButton_=new QPushButton(QStringLiteral("停止服务")); connect(serverButton_,&QPushButton::clicked,this,[this]{serverRunning_?stopServer():startServer();});
    endpointRow->addWidget(localUrl_,1); endpointRow->addWidget(copyLocal); endpointRow->addWidget(serverButton_); serverLayout->addLayout(endpointRow);
    auto *settings=new QFormLayout(); portSpin_=new QSpinBox(); portSpin_->setRange(1024,65535); portSpin_->setValue(saved.value("port",3333).toInt());
    authTokenEdit_=new QLineEdit(Credentials::readMcpToken()); authTokenEdit_->setEchoMode(QLineEdit::Password); authTokenEdit_->setPlaceholderText(QStringLiteral("留空仅本机访问免认证；公网建议设置"));
    settings->addRow(QStringLiteral("MCP 端口"),portSpin_); settings->addRow(QStringLiteral("Bearer 访问令牌"),authTokenEdit_); serverLayout->addLayout(settings);
    layout->addWidget(serverCard);

    auto *ngrokCard=card(root); auto *ngrokLayout=new QVBoxLayout(ngrokCard);
    auto *ngrokHead=new QHBoxLayout(); auto *ngrokTitle=new QLabel(QStringLiteral("公网访问 · ngrok")); ngrokTitle->setObjectName("sectionTitle");
    ngrokStatus_=new QLabel(QStringLiteral("未启动")); ngrokStatus_->setObjectName("muted"); ngrokHead->addWidget(ngrokTitle); ngrokHead->addStretch(); ngrokHead->addWidget(ngrokStatus_); ngrokLayout->addLayout(ngrokHead);
    auto *ngrokRow=new QHBoxLayout(); publicUrl_=new QLabel(QStringLiteral("设置 authtoken 后启动 ngrok")); publicUrl_->setObjectName("endpoint"); publicUrl_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *copyPublic=new QPushButton(QStringLiteral("复制地址")); connect(copyPublic,&QPushButton::clicked,this,&MainWindow::copyPublicUrl);
    ngrokButton_=new QPushButton(QStringLiteral("启动 ngrok")); connect(ngrokButton_,&QPushButton::clicked,this,&MainWindow::toggleNgrok);
    ngrokRow->addWidget(publicUrl_,1); ngrokRow->addWidget(copyPublic); ngrokRow->addWidget(ngrokButton_); ngrokLayout->addLayout(ngrokRow);
    auto *ngrokForm=new QFormLayout(); ngrokTokenEdit_=new QLineEdit(Credentials::readNgrokToken()); ngrokTokenEdit_->setEchoMode(QLineEdit::Password); ngrokTokenEdit_->setPlaceholderText(QStringLiteral("ngrok authtoken（保存到 Windows 凭据管理器）"));
    auto *domainRow=new QHBoxLayout(); ngrokDomainEdit_=new QLineEdit(saved.value("ngrokDomain").toString()); ngrokDomainEdit_->setPlaceholderText(QStringLiteral("可选：固定域名，例如 https://name.ngrok.app")); domainRow->addWidget(ngrokDomainEdit_);
    ngrokPathEdit_=new QLineEdit(saved.value("ngrokExecutable").toString()); ngrokPathEdit_->setPlaceholderText(QStringLiteral("留空自动查找 ngrok；Windows 首次启动可自动下载"));
    ngrokForm->addRow(QStringLiteral("Authtoken"),ngrokTokenEdit_); ngrokForm->addRow(QStringLiteral("固定域名"),domainRow); ngrokForm->addRow(QStringLiteral("ngrok.exe 路径"),ngrokPathEdit_); ngrokLayout->addLayout(ngrokForm);
    auto *ngrokHint=new QLabel(QStringLiteral("首次开启会从 ngrok 官方下载站获取 Agent；需 ngrok 账号令牌。令牌保存在系统凭据库，不写入配置文件。")); ngrokHint->setObjectName("muted"); ngrokHint->setWordWrap(true); ngrokLayout->addWidget(ngrokHint);
    layout->addWidget(ngrokCard);

    editorBridge_=new EditorBridge(root);
    layout->addWidget(editorBridge_, 1);

    auto *logCard=card(root); auto *logLayout=new QVBoxLayout(logCard); auto *logTitle=new QLabel(QStringLiteral("活动日志")); logTitle->setObjectName("sectionTitle"); logLayout->addWidget(logTitle);
    logView_=new QPlainTextEdit(); logView_->setReadOnly(true); logView_->setMaximumBlockCount(600); logView_->setMinimumHeight(115); logView_->setPlaceholderText(QStringLiteral("服务状态、MCP 请求与隧道事件会显示在这里。")); logLayout->addWidget(logView_); layout->addWidget(logCard,1);

    setCentralWidget(root);
    applyTheme();
}

void MainWindow::buildTray()
{
    tray_=new QSystemTrayIcon(trayIcon(),this); tray_->setToolTip(QStringLiteral("VS Code MCP Bridge · 启动中"));
    trayMenu_=new QMenu(this);
    QAction *open=trayMenu_->addAction(QStringLiteral("打开管理面板")); connect(open,&QAction::triggered,this,&MainWindow::showWindow);
    trayServerAction_=trayMenu_->addAction(QStringLiteral("停止本地服务")); connect(trayServerAction_,&QAction::triggered,this,[this]{serverRunning_?stopServer():startServer();});
    trayTunnelAction_=trayMenu_->addAction(QStringLiteral("启动 ngrok")); connect(trayTunnelAction_,&QAction::triggered,this,&MainWindow::toggleNgrok);
    trayMenu_->addSeparator(); QAction *quit=trayMenu_->addAction(QStringLiteral("退出")); connect(quit,&QAction::triggered,this,[this]{forceQuit_=true; QApplication::quit();});
    tray_->setContextMenu(trayMenu_);
    connect(tray_,&QSystemTrayIcon::activated,this,[this](QSystemTrayIcon::ActivationReason reason){if(reason==QSystemTrayIcon::DoubleClick)showWindow();});
    tray_->show();
}

void MainWindow::loadSettings()
{
    QSettings s;
    actualPort_=s.value("port",3333).toInt();
}

void MainWindow::saveSettings()
{
    if (editorBridge_) editorBridge_->setWorkspace(workspaceEdit_->text());
    const bool restartForPortChange = serverRunning_ && portSpin_->value() != actualPort_;
    QSettings s; s.setValue("workspace",workspaceEdit_->text()); s.setValue("port",portSpin_->value());
    s.setValue("ngrokDomain",ngrokDomainEdit_->text()); s.setValue("ngrokExecutable",ngrokPathEdit_->text());
    if(themeCombo_) s.setValue("theme",themeCombo_->currentData().toString());
    s.sync();
    if (authTokenEdit_->text().trimmed().isEmpty()) Credentials::clearMcpToken();
    else { QString credentialError; if (!Credentials::writeMcpToken(authTokenEdit_->text().trimmed(),&credentialError)) appendLog(credentialError); }
    if (server_) {
        QMetaObject::invokeMethod(server_,"setWorkspace",Qt::QueuedConnection,Q_ARG(QString,workspaceEdit_->text()));
        QMetaObject::invokeMethod(server_,"setAuthToken",Qt::QueuedConnection,Q_ARG(QString,authTokenEdit_->text()));
    }
    if (ngrok_) ngrok_->setExecutable(ngrokPathEdit_->text());
    if (restartForPortChange) {
        const bool restartTunnel = ngrokRunning_;
        if (restartTunnel) ngrok_->stop();
        QMetaObject::invokeMethod(server_,"stop",Qt::QueuedConnection);
        QMetaObject::invokeMethod(server_,"start",Qt::QueuedConnection,Q_ARG(int,portSpin_->value()));
        if (restartTunnel) {
            const QString token = Credentials::readNgrokToken();
            const QString domain = ngrokDomainEdit_->text();
            QTimer::singleShot(400,this,[this,token,domain]() {
                if(serverRunning_ && !token.isEmpty()) ngrok_->start(actualPort_,token,domain);
            });
        }
    }
}

void MainWindow::chooseWorkspace()
{
    const QString path=QFileDialog::getExistingDirectory(this,QStringLiteral("选择 MCP 工作区"),workspaceEdit_->text());
    if (!path.isEmpty()) { workspaceEdit_->setText(path); saveSettings(); appendLog(QStringLiteral("工作区：%1").arg(path)); }
}

QString MainWindow::localMcpUrl() const { return QStringLiteral("http://127.0.0.1:%1/mcp").arg(actualPort_); }

void MainWindow::startServer()
{
    saveSettings();
    if (serverRunning_) return;
    QMetaObject::invokeMethod(server_,"start",Qt::QueuedConnection,Q_ARG(int,portSpin_->value()));
}

void MainWindow::stopServer()
{
    if (!server_) return;
    if (ngrokRunning_) { ngrok_->stop(); ngrokRunning_=false; }
    QMetaObject::invokeMethod(server_,"stop",Qt::QueuedConnection);
}

void MainWindow::toggleNgrok()
{
    if (ngrokRunning_) { ngrok_->stop(); return; }
    if (!serverRunning_) startServer();
    const QString token=ngrokTokenEdit_->text().trimmed();
    if (token.isEmpty()) { QMessageBox::information(this,QStringLiteral("需要 ngrok authtoken"),QStringLiteral("前往 ngrok.com 注册并获取 authtoken，然后粘贴到 Authtoken 栏。")); return; }
    if (authTokenEdit_->text().trimmed().isEmpty()) {
        QMessageBox::warning(this,QStringLiteral("公网服务未启用认证"),QStringLiteral("ngrok 会把 MCP 服务公开到互联网。当前未设置 Bearer 访问令牌，任何知道公网地址的人都可能访问。将按原版行为继续启动；如需认证，请先设置访问令牌。"));
    }
    QString error;
    if (!Credentials::writeNgrokToken(token,&error)) { QMessageBox::warning(this,QStringLiteral("保存令牌失败"),error); return; }
    saveSettings();
    ngrok_->start(actualPort_,token,ngrokDomainEdit_->text());
}

void MainWindow::copyLocalUrl() { QApplication::clipboard()->setText(localMcpUrl()); appendLog(QStringLiteral("已复制本地 MCP 地址")); }
void MainWindow::copyPublicUrl() { if (ngrok_->publicUrl().isEmpty()) return; QApplication::clipboard()->setText(ngrok_->publicUrl()+QStringLiteral("/mcp")); appendLog(QStringLiteral("已复制公网 MCP 地址")); }

void MainWindow::appendLog(const QString &message)
{
    if (!logView_) return;
    logView_->appendPlainText(QStringLiteral("%1  %2").arg(QDateTime::currentDateTime().toString("HH:mm:ss"),message));
}

void MainWindow::updateServerState(bool running, int port, const QString &error)
{
    serverRunning_=running; if(port>0) actualPort_=port;
    if(running && portSpin_->value()!=port) portSpin_->setValue(port);
    serverStatus_->setText(error.isEmpty() ? (running?QStringLiteral("运行中"):QStringLiteral("已停止")) : QStringLiteral("启动失败"));
    serverStatus_->setStyleSheet(error.isEmpty() ? (running?"color:#15803d;font-weight:650":"color:#718096") : "color:#dc2626;font-weight:650");
    serverButton_->setText(running?QStringLiteral("停止服务"):QStringLiteral("启动服务"));
    localUrl_->setText(localMcpUrl()); updateTray();
    if (running && autoTunnelPending_ && ngrok_) {
        autoTunnelPending_ = false;
        appendLog(QStringLiteral("已检测到固定域名和访问令牌，自动启动 ngrok"));
        ngrok_->start(actualPort_, ngrokTokenEdit_->text().trimmed(), ngrokDomainEdit_->text().trimmed());
    }
    if (!error.isEmpty()) { appendLog(QStringLiteral("HTTP 服务启动失败：%1").arg(error)); QMessageBox::warning(this,QStringLiteral("MCP 服务启动失败"),QStringLiteral("%1\n\n请更换端口或关闭占用端口的程序。").arg(error)); }
}

void MainWindow::updateNgrokState(bool running, const QString &url)
{
    ngrokRunning_=running; ngrokStatus_->setText(running?(url.isEmpty()?QStringLiteral("正在连接…"):QStringLiteral("已连接")):QStringLiteral("未启动"));
    publicUrl_->setText(url.isEmpty()?QStringLiteral("设置 authtoken 后启动 ngrok"):url+QStringLiteral("/mcp"));
    ngrokButton_->setText(running?QStringLiteral("停止 ngrok"):QStringLiteral("启动 ngrok")); updateTray();
}

void MainWindow::updateTray()
{
    if (!tray_) return;
    tray_->setToolTip(QStringLiteral("VS Code MCP Bridge · %1 · %2").arg(serverRunning_?QStringLiteral("运行中"):QStringLiteral("已停止"),ngrokRunning_?QStringLiteral("ngrok 已连接"):QStringLiteral("本地服务")));
    if(trayServerAction_) trayServerAction_->setText(serverRunning_?QStringLiteral("停止本地服务"):QStringLiteral("启动本地服务"));
    if(trayTunnelAction_) trayTunnelAction_->setText(ngrokRunning_?QStringLiteral("停止 ngrok"):QStringLiteral("启动 ngrok"));
}

void MainWindow::showWindow() { showNormal(); activateWindow(); raise(); }

bool MainWindow::darkTheme() const
{
    if(themeCombo_ && themeCombo_->currentData().toString()==QStringLiteral("dark")) return true;
    if(themeCombo_ && themeCombo_->currentData().toString()==QStringLiteral("light")) return false;
    return QGuiApplication::styleHints()->colorScheme()==Qt::ColorScheme::Dark;
}

void MainWindow::applyTheme()
{
    const bool dark=darkTheme();
    const QString window=dark?QStringLiteral("#111827"):QStringLiteral("#f4f7fb");
    const QString panel=dark?QStringLiteral("#1f2937"):QStringLiteral("#ffffff");
    const QString field=dark?QStringLiteral("#111827"):QStringLiteral("#fbfcfe");
    const QString text=dark?QStringLiteral("#e5e7eb"):QStringLiteral("#24364b");
    const QString heading=dark?QStringLiteral("#f3f4f6"):QStringLiteral("#13243a");
    const QString muted=dark?QStringLiteral("#9ca3af"):QStringLiteral("#718096");
    const QString border=dark?QStringLiteral("#374151"):QStringLiteral("#e4eaf2");
    const QString inputBorder=dark?QStringLiteral("#4b5563"):QStringLiteral("#d8e1ec");
    const QString button=dark?QStringLiteral("#263244"):QStringLiteral("#f2f6fc");
    const QString hover=dark?QStringLiteral("#334155"):QStringLiteral("#e8f1ff");
    const QString blue=dark?QStringLiteral("#60a5fa"):QStringLiteral("#1769e0");
    const QString selection=dark?QStringLiteral("#1e40af"):QStringLiteral("#bcd7ff");
    QString css=QStringLiteral(R"(
        QMainWindow { background:%1; }
        QWidget#root { background:%1; }
        QWidget { color:%2; }
        QLabel#title { color:%3; font-size:24px; font-weight:700; }
        QLabel#muted { color:%4; font-size:12px; }
        QLabel#sectionTitle { color:%3; font-size:15px; font-weight:650; }
        QLabel#status { color:#22c55e; font-weight:650; }
        QLabel#endpoint { color:%5; font-family:Consolas,monospace; font-size:13px; }
        QFrame#card { background:%6; border:1px solid %7; border-radius:12px; }
        QLineEdit,QSpinBox,QComboBox,QPlainTextEdit { background:%8; color:%2; border:1px solid %9; border-radius:7px; padding:8px; selection-background-color:%10; }
        QLineEdit:focus,QSpinBox:focus,QComboBox:focus,QPlainTextEdit:focus { border:1px solid %5; }
        QPushButton { background:%11; color:%2; border:1px solid %9; border-radius:7px; padding:8px 13px; }
        QPushButton:hover { background:%12; border-color:%5; }
        QPushButton:pressed { background:%10; }
        QMenu { background:%6; color:%2; border:1px solid %7; }
        QMenu::item:selected { background:%12; }
        QToolTip { background:%6; color:%2; border:1px solid %7; padding:4px; }
    )").arg(window).arg(text).arg(heading).arg(muted).arg(blue).arg(panel)
        .arg(border).arg(field).arg(inputBorder).arg(selection).arg(button).arg(hover);
    setStyleSheet(css);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!forceQuit_ && QSystemTrayIcon::isSystemTrayAvailable()) { hide(); tray_->showMessage(QStringLiteral("VS Code MCP Bridge"),QStringLiteral("应用仍在托盘运行。右键托盘图标退出。"),QSystemTrayIcon::Information,2500); event->ignore(); return; }
    event->accept();
}
