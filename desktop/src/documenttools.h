#pragma once
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <functional>

// Portable, bounded document extraction. No Office installation required for
// OOXML archives; PDF extraction needs an optional pdftotext executable.
class DocumentTools
{
public:
    using PathGuard = std::function<QString(const QString &, bool)>;
    static QJsonValue read(const QJsonObject &arguments, const PathGuard &guard);
};
