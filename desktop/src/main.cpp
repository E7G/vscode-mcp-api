#include "mainwindow.h"

#include <QApplication>
#include <QIcon>
#include <QStyleFactory>
#include <QCommandLineParser>
#include <QDir>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("VS Code MCP Bridge");
    QApplication::setOrganizationName("Danchuna");
    QApplication::setApplicationVersion(APP_VERSION);
    QApplication::setQuitOnLastWindowClosed(false);

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("workspace"), QStringLiteral("Project directory to serve"), QStringLiteral("path")});
    parser.process(app);
    const QString workspace = parser.value(QStringLiteral("workspace"));
    MainWindow window(workspace.isEmpty() ? QString{} : QDir(workspace).absolutePath());
    window.show();
    return app.exec();
}
