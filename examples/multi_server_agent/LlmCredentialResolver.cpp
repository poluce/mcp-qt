#include "LlmCredentialResolver.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#ifdef Q_OS_WIN
#include <QSettings>
#endif

namespace mcp_agent {
namespace {

QString normalizedSecret(QString value)
{
    value = value.trimmed();
    if (value.size() >= 2
        && ((value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')))
            || (value.startsWith(QLatin1Char('\'')) && value.endsWith(QLatin1Char('\''))))) {
        value = value.mid(1, value.size() - 2).trimmed();
    }
    return value;
}

QString readDotEnvKey(const QString& keyName)
{
    const QStringList candidates = {
        QDir::currentPath() + QStringLiteral("/.env"),
        QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("../../.env")),
    };
    const QString prefix = keyName + QLatin1Char('=');
    for (const QString& path : candidates) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }
        while (!file.atEnd()) {
            const QString line = QString::fromUtf8(file.readLine()).trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
                continue;
            }
            if (line.startsWith(prefix)) {
                return normalizedSecret(line.mid(prefix.size()));
            }
        }
    }
    return {};
}

QString readWindowsEnvironmentKey(const QString& keyName)
{
#ifdef Q_OS_WIN
    // qEnvironmentVariable() only sees the environment inherited when this
    // process started. Reading these registry locations also sees variables
    // added after an IDE/terminal (or its parent process) was launched.
    const QSettings userEnvironment(
        QStringLiteral("HKEY_CURRENT_USER\\Environment"), QSettings::NativeFormat);
    QString value = normalizedSecret(userEnvironment.value(keyName).toString());
    if (!value.isEmpty()) {
        return value;
    }

    const QSettings machineEnvironment(
        QStringLiteral("HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment"),
        QSettings::NativeFormat);
    return normalizedSecret(machineEnvironment.value(keyName).toString());
#else
    Q_UNUSED(keyName);
    return {};
#endif
}

QString resolveNamedKey(const QString& keyName)
{
    QString value = normalizedSecret(qEnvironmentVariable(keyName.toUtf8().constData()));
    if (value.isEmpty()) {
        value = readDotEnvKey(keyName);
    }
    if (value.isEmpty()) {
        value = readWindowsEnvironmentKey(keyName);
    }
    return value;
}

} // namespace

QString resolveLlmApiKey()
{
    for (const QString& name : {
             QStringLiteral("DEEPSEEK_API_KEY"),
             QStringLiteral("DEEPSEEK"),
             QStringLiteral("OPENAI_API_KEY")}) {
        const QString value = resolveNamedKey(name);
        if (!value.isEmpty()) {
            return value;
        }
    }
    return {};
}

} // namespace mcp_agent
