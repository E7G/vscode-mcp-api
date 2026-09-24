#pragma once

#include <QMainWindow>
#include <QThread>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QComboBox;
class QSystemTrayIcon;
class QCloseEvent;
class QAction;
class QMenu;
class McpServer;
class NgrokManager;
class EditorBridge;

class MainWindow final : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(const QString &workspaceOverride = {}, QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void chooseWorkspace();
    void startServer();
    void stopServer();
    void toggleNgrok();
    void saveSettings();
    void copyLocalUrl();
    void copyPublicUrl();
    void appendLog(const QString &message);
    void updateServerState(bool running, int port, const QString &error);
    void updateNgrokState(bool running, const QString &url);

private:
    void buildUi();
    void buildTray();
    void loadSettings();
    void updateTray();
    void showWindow();
    void applyTheme();
    bool darkTheme() const;
    QString localMcpUrl() const;

    QThread serverThread_;
    McpServer *server_ = nullptr;
    EditorBridge *editorBridge_ = nullptr;
    NgrokManager *ngrok_ = nullptr;
    QSystemTrayIcon *tray_ = nullptr;
    QMenu *trayMenu_ = nullptr;
    QAction *trayServerAction_ = nullptr;
    QAction *trayTunnelAction_ = nullptr;
    QLineEdit *workspaceEdit_ = nullptr;
    QLineEdit *authTokenEdit_ = nullptr;
    QLineEdit *ngrokTokenEdit_ = nullptr;
    QLineEdit *ngrokDomainEdit_ = nullptr;
    QLineEdit *ngrokPathEdit_ = nullptr;
    QSpinBox *portSpin_ = nullptr;
    QComboBox *themeCombo_ = nullptr;
    QLabel *serverStatus_ = nullptr;
    QLabel *localUrl_ = nullptr;
    QLabel *ngrokStatus_ = nullptr;
    QLabel *publicUrl_ = nullptr;
    QPushButton *serverButton_ = nullptr;
    QPushButton *ngrokButton_ = nullptr;
    QPlainTextEdit *logView_ = nullptr;
    int actualPort_ = 3333;
    bool serverRunning_ = false;
    bool ngrokRunning_ = false;
    bool forceQuit_ = false;
    bool autoTunnelPending_ = false;
    QString workspaceOverride_;
};
