#include "credentials.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincred.h>
#endif

namespace Credentials {
namespace {
constexpr wchar_t NgrokTarget[] = L"Danchuna.VSCodeMcpBridge.Ngrok";
constexpr wchar_t McpTarget[] = L"Danchuna.VSCodeMcpBridge.Bearer";
QString read(const wchar_t *target)
{
#ifdef Q_OS_WIN
    PCREDENTIALW credential = nullptr;
    if (CredReadW(target, CRED_TYPE_GENERIC, 0, &credential)) {
        const QString result = QString::fromUtf8(reinterpret_cast<const char *>(credential->CredentialBlob),
            static_cast<qsizetype>(credential->CredentialBlobSize));
        CredFree(credential); return result;
    }
#else
    Q_UNUSED(target);
#endif
    return {};
}
bool write(const wchar_t *target, const QString &token, QString *error)
{
#ifdef Q_OS_WIN
    const QByteArray bytes = token.toUtf8();
    CREDENTIALW credential{}; credential.Type=CRED_TYPE_GENERIC;
    credential.TargetName=const_cast<LPWSTR>(target);
    credential.CredentialBlobSize=static_cast<DWORD>(bytes.size());
    credential.CredentialBlob=reinterpret_cast<LPBYTE>(const_cast<char *>(bytes.constData()));
    credential.Persist=CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName=const_cast<LPWSTR>(L"local application token");
    if (CredWriteW(&credential,0)) return true;
    if(error) *error=QStringLiteral("Windows Credential Manager 写入失败（%1）").arg(GetLastError());
    return false;
#else
    Q_UNUSED(target); Q_UNUSED(token);
    if(error) *error=QStringLiteral("当前平台尚未配置系统凭据库适配器");
    return false;
#endif
}
void clear(const wchar_t *target)
{
#ifdef Q_OS_WIN
    CredDeleteW(target,CRED_TYPE_GENERIC,0);
#else
    Q_UNUSED(target);
#endif
}
}

QString readNgrokToken() { return read(NgrokTarget); }
bool writeNgrokToken(const QString &token, QString *error) { return write(NgrokTarget,token,error); }
void clearNgrokToken() { clear(NgrokTarget); }
QString readMcpToken() { return read(McpTarget); }
bool writeMcpToken(const QString &token, QString *error) { return write(McpTarget,token,error); }
void clearMcpToken() { clear(McpTarget); }
}
