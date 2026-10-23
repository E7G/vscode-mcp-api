#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <functional>

// Workspace-limited filesystem operations. The caller supplies its canonical
// path guard so every operation shares the same symlink/traversal policy.
class WorkspaceTools
{
public:
    using PathGuard = std::function<QString(const QString &, bool)>;
    static bool supports(const QString &name);
    static QJsonValue call(const QString &name, const QJsonObject &args,
                           const QString &workspace, const PathGuard &guard);
};
