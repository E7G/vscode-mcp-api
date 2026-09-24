#pragma once

#include <QString>

namespace Credentials {
QString readNgrokToken();
bool writeNgrokToken(const QString &token, QString *error = nullptr);
void clearNgrokToken();
QString readMcpToken();
bool writeMcpToken(const QString &token, QString *error = nullptr);
void clearMcpToken();
}
