#include "installer.h"
#include "config.h"
#include "vpngui.h"

#include <stdexcept>

#include <QTextStream>
#include <QCryptographicHash>
#include <QDebug>
#include <QSysInfo>
#include <QCoreApplication>
#include <QMessageBox>

#define _WIN32_DCOM

#include "windows.h"
#include "winnls.h"
#include "shobjidl.h"
#include "objbase.h"
#include "objidl.h"
#include "shlguid.h"
#include <comdef.h>
#include <wincred.h>
#include <taskschd.h>


struct VersionFileStruct {
    QString name;
    QString version;
};

bool readVersionFile(const QString &path, VersionFileStruct &s) {
    QFile file(path);
    if (!file.open(QFile::ReadOnly)) {
        return false;
    }

    QString content(file.readAll());
    content = content.trimmed();

    QStringList parts(content.split(" "));
    if (parts.size() != 2) {
        return false;
    }

    s.name = parts[0];
    s.version = parts[1];
    return true;
}

// Split in "-", then ".", compare each part from left to right
// Supports integer and ascii comparison for each part.
bool versionHigherThan(const QString &va, const QString &vb) {
    if (va.contains("-") || vb.contains("-")) {
        QStringList aParts(va.split("-"));
        QStringList bParts(vb.split("-"));
        int maxParts = std::max(aParts.size(), bParts.size());
        for (int i=0; i<maxParts; i++) {
            if (aParts.size() <= i) {
                return false;
            }
            if (bParts.size() <= i) {
                return true;
            }
            if (aParts.at(i) == bParts.at(i)) {
                continue;
            }
            return versionHigherThan(aParts.at(i), bParts.at(i));
        }
        return false; // equal
    }
    else {
        QStringList aParts(va.split("."));
        QStringList bParts(vb.split("."));
        int maxParts = std::max(aParts.size(), bParts.size());
        for (int i=0; i<maxParts; i++) {
            if (aParts.size() <= i) {
                return false;
            }
            if (bParts.size() <= i) {
                return true;
            }
            QString aPart(aParts.at(i));
            QString bPart(bParts.at(i));

            if (aPart == bPart) {
                continue;
            }

            bool aIntOk, bIntOk;
            int aInt = aPart.toInt(&aIntOk, 10);
            int bInt = bPart.toInt(&bIntOk, 10);
            if (aIntOk && bIntOk) {
                return aInt > bInt;
            } else {
                return aPart > bPart;
            }
        }
        return false; // equal
    }
}


bool createLink(QString linkPath, QString destPath, QString desc) {
    HRESULT hres;
    IShellLink* psl;
    bool success = false;

    wchar_t* lpszPathObj = new wchar_t[destPath.length() + 1];
    destPath.toWCharArray(lpszPathObj);
    lpszPathObj[destPath.length()] = 0;

    wchar_t* lpszPathLink = new wchar_t[linkPath.length() + 1];
    linkPath.toWCharArray(lpszPathLink);
    lpszPathLink[linkPath.length()] = 0;

    wchar_t* lpszDesc = new wchar_t[desc.length() + 1];
    desc.toWCharArray(lpszDesc);
    lpszDesc[desc.length()] = 0;

    CoInitialize(NULL);

    // Get a pointer to the IShellLink interface. It is assumed that CoInitialize
    // has already been called.
    hres = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLink, (LPVOID*)&psl);
    if (SUCCEEDED(hres)) {
        IPersistFile* ppf;

        // Set the path to the shortcut target and add the description.
        psl->SetPath(lpszPathObj);
        psl->SetDescription(lpszDesc);

        // Query IShellLink for the IPersistFile interface, used for saving the
        // shortcut in persistent storage.
        hres = psl->QueryInterface(IID_IPersistFile, (LPVOID*)&ppf);

        if (SUCCEEDED(hres)) {
            // Save the link by calling IPersistFile::Save.
            hres = ppf->Save(lpszPathLink, TRUE);
            ppf->Release();

            success = true;
        }
        psl->Release();
    }

    delete[] lpszPathObj;
    delete[] lpszPathLink;
    delete[] lpszDesc;

    return success;
}

bool isTAPInstalled(bool w64) {
    HKEY hKey;
    REGSAM samDesired = KEY_READ;
    if (w64) {
        samDesired |= KEY_WOW64_64KEY;
    }
    LONG lRes = RegOpenKeyExW(HKEY_LOCAL_MACHINE, TEXT("SOFTWARE\\TAP-Windows"), 0, samDesired, &hKey);

    if (lRes != ERROR_SUCCESS) {
        qDebug() << "Failed to open key:" << lRes;
        return false;
    }

    WCHAR szBuffer[512];
    DWORD dwBufferSize = sizeof(szBuffer);
    ULONG nError;
    nError = RegQueryValueEx(hKey, NULL, 0, NULL, (LPBYTE)szBuffer, &dwBufferSize);
    if (ERROR_SUCCESS != nError) {
        qDebug() << "Failed to get value:" << nError;
        return false;
    }

    std::wstring tapPath(szBuffer);
    QDir binDir(QString::fromStdWString(tapPath));
    QString binPath(binDir.filePath("bin\\tapinstall.exe"));

    QStringList args;
    args.append("find");
    args.append("tap0901");

    QProcess tapTest;
    tapTest.setProcessChannelMode(QProcess::MergedChannels);
    tapTest.start(binPath, args);
    if (!tapTest.waitForFinished(3000)) {
        tapTest.kill();
    }

    QString output(QString::fromLocal8Bit(tapTest.readAll()));
    QStringList lines(output.split("\r\n", QString::SkipEmptyParts));
    qDebug() << lines;

    if (lines.empty()) {
        return false;
    }

    QString lastLine(lines.last());
    if (!lastLine.endsWith(" matching device(s) found.")) {
        return false;
    }

    QStringList words(lastLine.split(' '));
    QString nStr(words[0]);
    bool ok = false;
    int n = nStr.toInt(&ok);

    if (!ok || n < 1) {
        return false;
    }

    qDebug() << "TAP found!";
    return true;
}

QByteArray hashFile(QString path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    QCryptographicHash hasher(QCryptographicHash::Sha1);
    if (!hasher.addData(&f)) {
        return QByteArray();
    }
    return hasher.result();
}

Installer::Installer(const VPNGUI &vpngui)
    : m_vpngui(vpngui)
{
    m_baseDir = QDir(QDir(qgetenv("APPDATA")).filePath(VPNGUI_ORGNAME "/" + m_vpngui.getName()));
    loadIndex();
}

QString Installer::getArch() {
    QString arch = QSysInfo::currentCpuArchitecture();
    if (arch == "x86_64") {
        return "64";
    }
    if (arch == "i386") {
        return "32";
    }
    throw std::runtime_error("Unsupported arch: " + arch.toStdString());
}

Installer::State Installer::getInstallState() {
    // Check installed version and hash
    {
        QString appSrcPath = QCoreApplication::applicationFilePath();
        QString appLocPath = m_baseDir.filePath(VPNGUI_EXENAME);
        QString versionPath = m_baseDir.filePath("version.txt");
        VersionFileStruct v;
        if (!readVersionFile(versionPath, v)) {
            qDebug() << "Installer: failed to read version file: " << versionPath;
            return NotInstalled;
        }
        if (v.name != m_vpngui.getName()) {
            qDebug() << "Installer: version file: different name";
            return NotInstalled;
        }
        if (v.version == m_vpngui.getFullVersion()) {
            // Same version, check hash if not the same exe file
            if (appSrcPath != appLocPath) {
                QByteArray appSrcHash = hashFile(appSrcPath);
                QByteArray appLocHash = hashFile(appLocPath);
                if (appSrcHash.length() == 0 || appSrcHash != appLocHash) {
                    qDebug() << "Installer: same version, different hash for app binary";
                    return NotInstalled;
                }
            }
        }
        else if (versionHigherThan(m_vpngui.getFullVersion(), v.version)) {
            // This is a newer version
            qDebug() << "Installer: found older version";
            return NotInstalled;
        }
        else {
            // This is an older version.
            // What to do here? I don't know, really. Quit here and start the
            // newer one looks good enough for most users.
            qDebug() << "Installer: version file: higher version found";
            return HigherVersionFound;
        }
    }

    // Check OpenVPN files hashes
    QMap<QString, QString>::iterator it;
    for(it=m_index.begin(); it != m_index.end(); ++it) {
        QString filename = it.key();
        QString hash = it.value();

        QString path = m_baseDir.filePath(filename);
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            qDebug() << "Installer: cannot open: " << path;
            return NotInstalled;
        }
        QCryptographicHash hasher(QCryptographicHash::Sha1);
        if (!hasher.addData(&f)) {
            qDebug() << "Installer: cannot hash: " << path;
            return NotInstalled;
        }
        QString new_hash = QString(hasher.result().toHex());
        if (new_hash != hash) {
            qDebug() << "Installer: different hash for " << path << ": " << new_hash;
            return NotInstalled;
        }
    }

    // Check TAP
    if (!isTAPInstalled(getArch() == "64")) {
        qDebug() << "Installer: TAP not installed";
        return NotInstalled;
    }

    return Installed;
}

Installer::State Installer::install() {
    if (!m_baseDir.exists()) {
        if (!m_baseDir.mkpath(".")) {
            throw std::runtime_error("Cannot mkdir: " + m_baseDir.path().toStdString());
        }
    }

    QString appSrcPath = QCoreApplication::applicationFilePath();
    QString appLocPath = m_baseDir.filePath(VPNGUI_EXENAME);

    // Create the version.txt file with current name, version and hash
    {
        QString versionPath(m_baseDir.filePath("version.txt"));
        QFile versionFile(versionPath);
        if (!versionFile.open(QFile::WriteOnly)) {
            throw std::runtime_error("Cannot write file: " + versionPath.toStdString());
        }
        QString content(m_vpngui.getName() + " " + m_vpngui.getFullVersion());
        versionFile.write(content.toUtf8());
        versionFile.close();
    }


    // Copy OpenVPN files listed in this arch's index.txt
    {
        QMap<QString, QString>::iterator it;
        for(it=m_index.begin(); it != m_index.end(); ++it) {
            QString filename = it.key();
            QString resPath(":/bin" + getArch() + "/" + filename);
            QString locPath(m_baseDir.filePath(filename));

            QFile resFile(resPath);
            if (!resFile.open(QIODevice::ReadOnly)) {
                throw std::runtime_error("Cannot read file: " + resPath.toStdString() + " -> " + locPath.toStdString());
            }

            QFile locFile(locPath);
            if (!locFile.open(QIODevice::WriteOnly)) {
                throw std::runtime_error("Cannot write file: " + resPath.toStdString() + " -> " + locPath.toStdString());
            }

            locFile.write(resFile.readAll());

            locFile.close();
            resFile.close();
        }
    }

    // Copy current binary there too, if it's not the same
    if (appSrcPath != appLocPath) {
        if (QFile(appLocPath).exists()) {
            while (!QFile(appLocPath).remove()) {
                QMessageBox::StandardButton r;
                QString msg(QCoreApplication::tr("%1 is already running. Please close it to upgrade."));
                msg = msg.arg(m_vpngui.getDisplayName());
                r = QMessageBox::warning(NULL, m_vpngui.getDisplayName(), msg,
                                         QMessageBox::Cancel | QMessageBox::Ok);
                if (r == QMessageBox::Cancel) {
                    return NotInstalled;
                }
            }
            if (QFile(appLocPath).exists() && !QFile(appLocPath).remove()) {
                throw std::runtime_error("Cannot delete for upgrade: " + appLocPath.toStdString());
            }
        }
        if (!QFile::copy(appSrcPath, appLocPath)) {
            throw std::runtime_error("Cannot copy file: " + appSrcPath.toStdString() + " -> " + appLocPath.toStdString());
        }
    }

    // Start TAP installation
    // I'd like to make this silent (/S) but since it may take some time and
    // ask a confirmation for the driver, I think it's better to show something.
    if (!isTAPInstalled(getArch() == "64")) {
        QProcess tapInstaller;
        tapInstaller.start(m_baseDir.filePath("tap-windows.exe"));
        tapInstaller.waitForFinished(-1);
    }

    // Make Start menu shortcut
    {
        QDir appdataDir(QString(qgetenv("APPDATA")));
        QDir startMenuDir(appdataDir.filePath("Microsoft\\Windows\\Start Menu\\Programs"));
        if (!startMenuDir.exists()) {
            startMenuDir.mkpath(".");
        }
        QString shortcut(startMenuDir.filePath(m_vpngui.getDisplayName() + ".lnk"));
        if (!createLink(shortcut, appLocPath, m_vpngui.getDisplayName())) {
            throw std::runtime_error("Failed to create start menu link: " + shortcut.toStdString() + " -> " + appLocPath.toStdString());
        }
    }

    // Make a desktop shortcut
    {
        QString lnkMsg(QCoreApplication::tr("%1 has been installed. Create a desktop shortcut?"));
        lnkMsg = lnkMsg.arg(m_vpngui.getDisplayName());
        QMessageBox::StandardButton r = QMessageBox::question(NULL, m_vpngui.getDisplayName(), lnkMsg);
        if (r == QMessageBox::Yes) {
            QDir homeDir(QString(qgetenv("USERPROFILE")));
            QDir desktopDir(homeDir.filePath("Desktop"));
            QString shortcut(desktopDir.filePath(m_vpngui.getDisplayName() + ".lnk"));
            if (!createLink(shortcut, appLocPath, m_vpngui.getDisplayName())) {
                throw std::runtime_error("Failed to create link: " + shortcut.toStdString() + " -> " + appLocPath.toStdString());
            }
        }
    }

    return Installed;
}

void Installer::uninstall(bool waitForOpenVPN) {
    QString appExePath = m_baseDir.filePath(VPNGUI_EXENAME);

    if (!m_baseDir.exists()) {
        return;
    }

    if (waitForOpenVPN) {
        // Try to delete openvpn.exe for up to a second
        // Even if it's killed before, the file is somehow still opened
        // at this point.
        for (int i=0; i<10; i++) {
            QFile f(m_baseDir.filePath("openvpn.exe"));
            if (!f.exists() || f.remove()) {
                break;
            }
            Sleep(100);
        }
    }

    m_baseDir.removeRecursively();

    // Now the desktop shortcut
    {
        QDir homeDir(QString(qgetenv("USERPROFILE")));
        QDir desktopDir(homeDir.filePath("Desktop"));
        QString shortcutPath(desktopDir.filePath(m_vpngui.getDisplayName() + ".lnk"));
        QFile shortcut(shortcutPath);
        if (shortcut.symLinkTarget() == appExePath) {
            shortcut.remove();
        }
    }

    // And the start menu shortcut
    {
        QDir appdataDir(QString(qgetenv("APPDATA")));
        QDir startMenuDir(appdataDir.filePath("Microsoft\\Windows\\Start Menu\\Programs"));
        QFile shortcut(startMenuDir.filePath(m_vpngui.getDisplayName() + ".lnk"));
        if (shortcut.symLinkTarget() == appExePath) {
            shortcut.remove();
        }
    }

    // The main .exe and .lock files should still exist now
    // they have to be deleted later.
    {
        QStringList toDelete;
        toDelete << appExePath
                 << m_baseDir.filePath("lvpngui.lock")
                 << m_baseDir.path()
                 << m_baseDir.dirName();

        foreach (QString path, toDelete) {
            std::wstring stdwsExistingFile(path.toStdWString());
            const wchar_t *szExistingFile = stdwsExistingFile.c_str();
            MoveFileEx(szExistingFile, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
    }
}

void Installer::loadIndex() {
    QFile index(":/bin" + getArch() + "/index.txt");
    if (!index.open(QIODevice::ReadOnly | QIODevice::Text)) {
        throw std::runtime_error("Cannot open index file");
    }
    QTextStream in(&index);
    in.setCodec("UTF-8");
    while (!in.atEnd()) {
        QString line = in.readLine().toUtf8();
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith('#')) {
            continue;
        }
        QStringList parts = line.split(" ");
        if (parts.size() != 2) {
            throw std::runtime_error("found invalid index entry");
        }
        m_index.insert(parts[1], parts[0]);
    }
    index.close();
}

bool Installer::setStartOnBoot(bool enabled) {
    QString program("schtasks");
    QString taskName(m_vpngui.getName() + "StartTask");
    QString taskRun = m_baseDir.filePath(VPNGUI_EXENAME);

    QFile tpl(":/schtasks_template.xml");
    if (!tpl.open(QFile::ReadOnly | QFile::Text)) {
        throw std::runtime_error("Cannot read schtasks_template.xml");
    }

    QString xml(QString::fromUtf8(tpl.readAll()));
    xml.replace("%FULLUSERNAME%", qgetenv("USERDOMAIN") + "\\" + qgetenv("USERNAME"));
    xml.replace("%EXEPATH%", taskRun);
    xml.replace("%TASKNAME%", taskName);

    QString xmlPath(m_baseDir.filePath("schtasks.xml"));
    QFile xmlFile(xmlPath);
    if (!xmlFile.open(QFile::WriteOnly | QFile::Text)) {
        throw std::runtime_error("Cannot write schtasks.xml");
    }
    xmlFile.write(xml.toUtf8());
    xmlFile.close();


    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    QStringList createArgs, deleteArgs;

    /*
    createArgs << "/Create"
               << "/RU" << qgetenv("USERNAME")
               << "/SC" << "ONLOGON"
               << "/TN" << taskName
               << "/TR" << taskRun
               << "/RL" << "HIGHEST"
               << "/IT";
    */

    createArgs << "/Create" << "/XML" << xmlPath
               << "/RU" << qgetenv("USERNAME")
               << "/TN" << taskName
               << "/IT";

    deleteArgs << "/Delete"
               << "/TN" << taskName << "/F";

    p.start(program, deleteArgs);
    p.waitForFinished(1000);
    p.kill();

    if (enabled) {
        p.start(program, createArgs);
        p.waitForFinished(3000);
        p.kill();

        if (p.exitCode() != 0) {
            QMessageBox::critical(NULL,
                                  "schtask error: " + QString::number(p.exitCode()),
                                  QString::fromLocal8Bit(p.readAll()));
            return false;
        }
    }
    return true;
}
