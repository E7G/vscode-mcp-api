#include "mainwindow.h"
#include "mcpserver.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QTextStream>
#include <cstring>
#include <memory>

int main(int argc, char *argv[])
{
    bool headless = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--headless") == 0) { headless = true; break; }
    }
    // Core-only mode does not load a display plugin, so SSH/headless servers
    // also work on Linux without a running X11/Wayland session.
    std::unique_ptr<QCoreApplication> app;
    if (headless) app = std::make_unique<QCoreApplication>(argc, argv);
    else app = std::make_unique<QApplication>(argc, argv);
    QCoreApplication::setApplicationName("VS Code MCP Bridge");
    QCoreApplication::setOrganizationName("Danchuna");
    QCoreApplication::setApplicationVersion(APP_VERSION);
    if (!headless) QApplication::setQuitOnLastWindowClosed(false);

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("workspace"), QStringLiteral("Project directory to serve"), QStringLiteral("path")});
    parser.addOption({QStringLiteral("headless"), QStringLiteral("Run MCP without the desktop window (for services and tests)")});
    parser.addOption({QStringLiteral("port"), QStringLiteral("HTTP port, used with --headless"), QStringLiteral("port"), QStringLiteral("3333")});
    parser.process(*app);
    const QString workspace = parser.value(QStringLiteral("workspace"));
    if (headless) {
        if (workspace.isEmpty() || QDir(workspace).canonicalPath().isEmpty()) {
            QTextStream(stderr) << "--headless requires an existing --workspace directory\n";
            return 2;
        }
        bool validPort = false;
        const int port = parser.value(QStringLiteral("port")).toInt(&validPort);
        if (!validPort || port < 1024 || port > 65535) {
            QTextStream(stderr) << "Invalid --port\n";
            return 2;
        }
        McpServer server;
        server.setWorkspace(workspace);
        server.setAuthToken(qEnvironmentVariable("VSCODE_MCP_AUTH_TOKEN"));
        QObject::connect(&server, &McpServer::serverStateChanged, app.get(),
            [&app](bool running, int, const QString &error) {
                if (!running && !error.isEmpty()) {
                    QTextStream(stderr) << error << '\n';
                    app->exit(1);
                }
            });
        server.start(port);
        return app->exec();
    }
    MainWindow window(workspace.isEmpty() ? QString{} : QDir(workspace).absolutePath());
    window.show();
    return app->exec();
}
