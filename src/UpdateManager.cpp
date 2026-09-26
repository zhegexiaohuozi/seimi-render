// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#include "UpdateManager.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSaveFile>
#include <QSysInfo>
#include <QTimer>

#include <cstdio>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <time.h>
#endif

#ifndef SEIMI_VERSION
#define SEIMI_VERSION "unknown"
#endif
#ifndef SEIMI_BUILD_TIME
#define SEIMI_BUILD_TIME "unknown"
#endif
#ifndef SEIMI_GIT_COMMIT
#define SEIMI_GIT_COMMIT "unknown"
#endif

namespace seimi {
namespace {

// 简化 semver 比较：只比 major.minor.patch（构建后缀忽略）。解析失败返回 false（视为不可比）。
bool versionGreaterThan(const QString& candidate, const QString& baseline) {
    auto parse = [](const QString& s) -> std::optional<std::tuple<int, int, int>> {
        const QStringList parts = s.trimmed().split(QLatin1Char('.'));
        if (parts.size() < 2) return std::nullopt;
        std::tuple<int, int, int> r{0, 0, 0};
        auto num = [](const QString& p) -> std::optional<int> {
            QString digits;
            for (const QChar& c : p) {
                if (c.isDigit()) digits += c;
                else break;
            }
            bool ok = false;
            const int v = digits.toInt(&ok);
            return ok ? std::optional<int>(v) : std::nullopt;
        };
        const auto maj = num(parts.value(0)), min = num(parts.value(1));
        if (!maj || !min) return std::nullopt;
        std::get<0>(r) = *maj; std::get<1>(r) = *min;
        if (parts.size() >= 3) {
            if (const auto pat = num(parts.value(2))) std::get<2>(r) = *pat;
        }
        return r;
    };
    const auto a = parse(candidate), b = parse(baseline);
    if (!a || !b) return false;
    return *a > *b;
}

QString platformKey() {
    QString product = QSysInfo::productType(); // "windows" | "osx"/"macos" | "linux"
    if (product == QLatin1String("osx")) product = QStringLiteral("macos");
    QString arch = QSysInfo::currentCpuArchitecture(); // "x86_64" | "arm64"
    if (arch == QLatin1String("x86_64")) arch = QStringLiteral("x64");
    return product + QLatin1Char('-') + arch;
}

// manifest 资产键的候选顺序（macos 允许 universal 包兜底）。
QStringList assetKeyCandidates() {
    const QString key = platformKey();
    QStringList candidates{key};
    if (key.startsWith(QLatin1String("macos-"))) candidates << QStringLiteral("macos-universal");
    return candidates;
}

QString humanSize(qint64 bytes) {
    if (bytes <= 0) return QStringLiteral("—");
    const double mb = double(bytes) / (1024.0 * 1024.0);
    if (mb >= 1024.0) return QString::number(mb / 1024.0, 'f', 2) + QStringLiteral(" GB");
    return QString::number(mb, 'f', 1) + QStringLiteral(" MB");
}

QString stateToString(UpdateManager::State s) {
    switch (s) {
        case UpdateManager::State::Idle:        return QStringLiteral("idle");
        case UpdateManager::State::Checking:    return QStringLiteral("checking");
        case UpdateManager::State::UpToDate:    return QStringLiteral("up-to-date");
        case UpdateManager::State::Available:   return QStringLiteral("available");
        case UpdateManager::State::Downloading: return QStringLiteral("downloading");
        case UpdateManager::State::Verifying:   return QStringLiteral("verifying");
        case UpdateManager::State::Ready:       return QStringLiteral("ready");
        case UpdateManager::State::Applying:    return QStringLiteral("applying");
        case UpdateManager::State::Error:       return QStringLiteral("error");
    }
    return QStringLiteral("idle");
}

// 目录 rename 带重试（Windows 上父进程退出/杀软扫描会让句柄短暂滞留）。
bool renameDirWithRetry(const QString& from, const QString& to, int attempts, int intervalMs) {
    for (int i = 0; i < attempts; ++i) {
        if (QDir().rename(from, to)) return true;
#ifdef Q_OS_WIN
        ::Sleep(intervalMs);
#else
        struct timespec ts{0, intervalMs * 1000L * 1000L};
        nanosleep(&ts, nullptr);
#endif
    }
    return false;
}

} // namespace

UpdateManager::UpdateManager(QObject* parent)
    : QObject(parent)
    , m_currentVersion(QString::fromLatin1(SEIMI_VERSION))
    , m_buildTime(QString::fromLatin1(SEIMI_BUILD_TIME))
    , m_gitCommit(QString::fromLatin1(SEIMI_GIT_COMMIT))
{
    m_nam = new QNetworkAccessManager(this);
    m_checkTimeout.setSingleShot(true);
    m_checkTimeout.setInterval(20 * 1000);
    connect(&m_checkTimeout, &QTimer::timeout, this, [this]() {
        if (m_state == State::Checking && m_reply) {
            m_reply->abort(); // 触发 error → 统一收口
        }
    });
}

UpdateManager::~UpdateManager() {
    if (m_dlFile) {
        m_dlFile->close();
        delete m_dlFile;
    }
    delete m_hash;
}

void UpdateManager::init(const QString& dataDir) {
    m_dataDir = dataDir;
    m_installerMode = detectInstallerMode(QCoreApplication::applicationDirPath());
    QDir().mkpath(dataDir + QStringLiteral("/updates"));
}

void UpdateManager::maybeAutoCheck(bool enabled) {
    if (!enabled || m_manifestUrl.trimmed().isEmpty()) return;
    QTimer::singleShot(8000, this, &UpdateManager::checkNow); // 启动后稍等，不抢首屏
}

bool UpdateManager::busy() const {
    return m_state == State::Checking || m_state == State::Downloading
        || m_state == State::Verifying || m_state == State::Applying;
}

QString UpdateManager::state() const { return stateToString(m_state); }

QString UpdateManager::packageSizeText() const {
    if (m_state == State::Downloading || m_state == State::Verifying || m_state == State::Ready) {
        return humanSize(m_assetSize);
    }
    return humanSize(m_assetSize); // Available 态也可能来自 manifest 声明
}

void UpdateManager::setManifestUrl(const QString& url) {
    const QString trimmed = url.trimmed();
    if (trimmed == m_manifestUrl) return;
    m_manifestUrl = trimmed;
    emit manifestUrlChanged();
}

void UpdateManager::setState(State s, const QString& error) {
    m_state = s;
    m_errorMessage = error;
    emit stateChanged();
}

void UpdateManager::resetAssetState() {
    m_verifiedPath.clear();
    m_progress = 0.0;
    m_progressText.clear();
    m_received = 0;
    emit progressChanged();
}

void UpdateManager::checkNow() {
    if (busy()) return;
    resetAssetState();
    m_availableVersion.clear();
    m_notesZh.clear();
    m_notesEn.clear();
    m_assetUrl.clear();
    m_assetSha256.clear();
    m_assetSize = 0;

    if (m_manifestUrl.trimmed().isEmpty()) {
        setState(State::Error, QStringLiteral("未配置更新清单地址（manifest URL）——请在下方填写后保存。"));
        return;
    }

    QNetworkRequest req{QUrl(m_manifestUrl.trimmed())};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(20 * 1000);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("seimi-render-updater/%1").arg(m_currentVersion));

    setState(State::Checking);
    m_reply = m_nam->get(req);
    m_checkTimeout.start();
    connect(m_reply, &QNetworkReply::finished, this, &UpdateManager::onManifestReceived);
}

void UpdateManager::onManifestReceived() {
    QNetworkReply* reply = m_reply;
    m_reply = nullptr;
    m_checkTimeout.stop();
    if (!reply) return;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        setState(State::Error, QStringLiteral("检查更新失败：%1").arg(reply->errorString()));
        return;
    }
    const QByteArray body = reply->readAll();
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        setState(State::Error, QStringLiteral("更新清单不是合法 JSON：%1").arg(err.errorString()));
        return;
    }
    const QJsonObject root = doc.object();
    const QString version = root.value(QStringLiteral("version")).toString().trimmed();
    if (version.isEmpty()) {
        setState(State::Error, QStringLiteral("更新清单缺少 version 字段。"));
        return;
    }
    m_notesZh = root.value(QStringLiteral("notes_zh")).toString();
    m_notesEn = root.value(QStringLiteral("notes_en")).toString();

    if (!versionGreaterThan(version, m_currentVersion)) {
        setState(State::UpToDate);
        return;
    }

    // 选资产：安装版优先 installer，绿色版 portable；按平台键候选顺序找。
    const QJsonObject assets = root.value(QStringLiteral("assets")).toObject();
    QJsonObject chosen;
    QString kind;
    for (const QString& key : assetKeyCandidates()) {
        const QJsonObject plat = assets.value(key).toObject();
        if (plat.isEmpty()) continue;
        const QJsonObject inst = plat.value(QStringLiteral("installer")).toObject();
        const QJsonObject port = plat.value(QStringLiteral("portable")).toObject();
        if (m_installerMode && inst.contains(QStringLiteral("url"))) {
            chosen = inst; kind = QStringLiteral("installer"); break;
        }
        if (port.contains(QStringLiteral("url"))) {
            chosen = port; kind = QStringLiteral("portable"); break;
        }
    }
    if (chosen.isEmpty() || kind.isEmpty()) {
        setState(State::Error,
                 QStringLiteral("更新清单没有当前平台（%1）的%2资产。")
                     .arg(platformKey(), m_installerMode ? QStringLiteral("安装包")
                                                         : QStringLiteral("绿色包")));
        return;
    }
    m_availableVersion = version;
    m_assetKind = kind;
    m_assetUrl = chosen.value(QStringLiteral("url")).toString();
    m_assetSha256 = chosen.value(QStringLiteral("sha256")).toString().toLower();
    m_assetSize = qint64(chosen.value(QStringLiteral("size")).toDouble(0));
    setState(State::Available);
}

void UpdateManager::startDownload() {
    if (m_state != State::Available && m_state != State::Error) return; // 失败态也允许重试下载
    if (m_assetUrl.isEmpty()) { checkNow(); return; }

    QDir().mkpath(m_dataDir + QStringLiteral("/updates"));
    const QString fileName = QStringLiteral("seimi-render-") + m_availableVersion
        + (m_assetKind == QLatin1String("installer") ? QStringLiteral(".exe")
                                                     : QStringLiteral(".archive"));
    const QString filePath = m_dataDir + QStringLiteral("/updates/") + fileName;
    if (QFile::exists(filePath)) QFile::remove(filePath);

    m_dlFile = new QFile(filePath);
    if (!m_dlFile->open(QIODevice::WriteOnly)) {
        delete m_dlFile; m_dlFile = nullptr;
        setState(State::Error, QStringLiteral("无法创建下载文件：%1").arg(filePath));
        return;
    }
    delete m_hash;
    m_hash = new QCryptographicHash(QCryptographicHash::Sha256);
    m_received = 0;
    m_lastProgressMs = 0;
    m_lastProgressBytes = 0;
    m_progress = 0.0;
    m_progressText = QStringLiteral("连接中…");
    emit progressChanged();

    QNetworkRequest req{QUrl(m_assetUrl)};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    setState(State::Downloading);
    m_reply = m_nam->get(req);
    connect(m_reply, &QNetworkReply::readyRead, this, &UpdateManager::onDownloadReadyRead);
    connect(m_reply, &QNetworkReply::downloadProgress, this, &UpdateManager::onDownloadProgress);
    connect(m_reply, &QNetworkReply::finished, this, &UpdateManager::onDownloadFinished);
}

void UpdateManager::cancelDownload() {
    if (m_state != State::Downloading || !m_reply) return;
    m_reply->abort(); // finished 里按「用户取消」收口
}

void UpdateManager::onDownloadReadyRead() {
    if (!m_reply || !m_dlFile) return;
    const QByteArray chunk = m_reply->readAll();
    m_dlFile->write(chunk);
    if (m_hash) m_hash->addData(chunk);
    m_received += chunk.size();
}

void UpdateManager::onDownloadProgress(qint64 received, qint64 total) {
    // 网络 emit 的值与本地累计可能短暂不一致，以网络侧为准展示（总量可信）。
    if (total > 0) {
        m_progress = double(received) / double(total) * 100.0;
        if (total > m_assetSize) m_assetSize = total;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_lastProgressMs > 0 && now > m_lastProgressMs) {
        const double bytesPerSec = double(received - m_lastProgressBytes) * 1000.0
                                 / double(now - m_lastProgressMs);
        m_progressText = QStringLiteral("%1 / %2 · %3/s")
            .arg(humanSize(received)).arg(humanSize(total)).arg(humanSize(qint64(bytesPerSec)));
    } else {
        m_progressText = QStringLiteral("%1 / %2").arg(humanSize(received)).arg(humanSize(total));
    }
    m_lastProgressMs = now;
    m_lastProgressBytes = received;
    emit progressChanged();
}

void UpdateManager::onDownloadFinished() {
    QNetworkReply* reply = m_reply;
    m_reply = nullptr;
    const bool userCancelled = (m_state == State::Downloading && reply && reply->error() == QNetworkReply::OperationCanceledError);
    if (reply) reply->deleteLater();

    if (m_dlFile) {
        m_dlFile->close();
        delete m_dlFile;
        m_dlFile = nullptr;
    }
    if (userCancelled) {
        // 取消 ≠ 跳过：退回 Available，保留清单信息供稍后重试（对齐桌面端更新器教训）。
        if (m_state == State::Downloading) setState(State::Available);
        return;
    }
    if (reply && reply->error() != QNetworkReply::NoError) {
        setState(State::Error, QStringLiteral("下载失败：%1").arg(reply->errorString()));
        return;
    }
    finalizeAsset();
}

void UpdateManager::finalizeAsset() {
    setState(State::Verifying);
    if (m_dlFile) { m_dlFile->close(); delete m_dlFile; m_dlFile = nullptr; }

    const QString fileName = QStringLiteral("seimi-render-") + m_availableVersion
        + (m_assetKind == QLatin1String("installer") ? QStringLiteral(".exe")
                                                     : QStringLiteral(".archive"));
    const QString filePath = m_dataDir + QStringLiteral("/updates/") + fileName;

    if (!m_assetSha256.isEmpty()) {
        QFile f(filePath);
        if (!f.open(QIODevice::ReadOnly)) {
            setState(State::Error, QStringLiteral("校验失败：无法读取下载文件。"));
            return;
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&f)) {
            setState(State::Error, QStringLiteral("校验失败：读取中断。"));
            return;
        }
        if (hash.result().toHex() != m_assetSha256) {
            QFile::remove(filePath);
            // 校验失败文件已删，可直接重试下载（Error 态允许 startDownload 重试）。
            std::fprintf(stderr, "[update] sha256 mismatch: expect %s got %s\n",
                         qPrintable(m_assetSha256),
                         hash.result().toHex().constData());
            setState(State::Error,
                     QStringLiteral("校验失败：SHA-256 不匹配（文件已删除），请重试下载。"));
            return;
        }
    } else {
        std::fprintf(stderr, "[update] manifest 未提供 sha256，跳过校验（不推荐）。\n");
    }
    m_verifiedPath = filePath;
    m_progress = 100.0;
    m_progressText = QStringLiteral("已就绪：%1").arg(QFileInfo(filePath).fileName());
    emit progressChanged();
    setState(State::Ready);
}

void UpdateManager::applyAndRestart() {
    if (m_state != State::Ready || !QFile::exists(m_verifiedPath)) return;
    setState(State::Applying);
    if (!spawnApplyChild()) {
        setState(State::Error, QStringLiteral("交接更新进程失败（详见控制台日志）。"));
        return;
    }
    emit requestQuit();
}

bool UpdateManager::extractArchive() {
    const QString extractRoot = m_dataDir + QStringLiteral("/updates/staged-") + m_availableVersion;
    QDir(extractRoot).removeRecursively();
    QDir().mkpath(extractRoot);

    // 统一用系统 tar（Win10+/macOS/Linux 均自带；bsdtar 可解 zip 与 tar.gz）。
    QProcess tar;
    tar.start(QStringLiteral("tar"),
              {QStringLiteral("-xf"), m_verifiedPath, QStringLiteral("-C"), extractRoot});
    if (!tar.waitForStarted(5000) || !tar.waitForFinished(180 * 1000)
        || tar.exitStatus() != QProcess::NormalExit || tar.exitCode() != 0) {
        std::fprintf(stderr, "[update] 解包失败: %s\n",
                     tar.readAllStandardError().constData());
        return false;
    }
    return true;
}

bool UpdateManager::spawnApplyChild() {
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString exeName = QFileInfo(QCoreApplication::applicationFilePath()).fileName();

    if (m_assetKind == QLatin1String("installer")) {
        // 安装版：NSIS /S 静默重装。安装器独立进程运行，本进程直接退出让位。
        QProcess installer;
        installer.setProgram(m_verifiedPath);
        installer.setArguments({QStringLiteral("/S")});
        if (!installer.startDetached()) return false;
        return true;
    }

    // 绿色版：解包 → 定位 payload → spawn apply 子进程。
    if (!extractArchive()) return false;
    const QString extractRoot = m_dataDir + QStringLiteral("/updates/staged-") + m_availableVersion;

    // 定位 payload：优先「与 appDir 同名」的目录（可移植包规范名），退而求其次唯一子目录。
    const QString targetName = QFileInfo(appDir).fileName();
    QString payload = extractRoot + QLatin1Char('/') + targetName;
    if (!QDir(payload).exists()) {
        const QStringList entries = QDir(extractRoot).entryList(
            QDir::Dirs | QDir::NoDotAndDotDot);
        if (entries.size() == 1) {
            payload = extractRoot + QLatin1Char('/') + entries.first();
        } else {
            std::fprintf(stderr, "[update] staged payload: target %s not found (%d subdirs)\n",
                         qPrintable(targetName), int(entries.size()));
            return false;
        }
    }

    QStringList args{QStringLiteral("--apply-update"), payload, appDir, exeName};
#ifdef Q_OS_WIN
    // Windows：运行中的 exe 所在目录不可 rename。把自身复制到 updates/ 目录再以副本
    // 启动 apply（副本不在 appDir 内）。残留副本由下次启动 cleanupUpdateResidue 清理。
    const QString helper = m_dataDir + QStringLiteral("/updates/updater.exe");
    if (QFile::exists(helper)) QFile::remove(helper);
    if (!QFile::copy(QCoreApplication::applicationFilePath(), helper)) {
        std::fprintf(stderr, "[update] 无法复制 updater 副本到 %s\n", qPrintable(helper));
        return false;
    }
    QProcess child;
    child.setProgram(helper);
    child.setArguments(args);
    return child.startDetached();
#else
    QProcess child;
    child.setProgram(QCoreApplication::applicationFilePath());
    child.setArguments(args);
    return child.startDetached();
#endif
}

// ======================= --apply-update 子进程 =======================

int runApplyUpdate(const QStringList& args) {
    // args: --apply-update <payload> <appDir> <exeName>
    if (args.size() < 4) {
        std::fprintf(stderr, "[apply-update] 参数不足: --apply-update <payload> <appDir> <exeName>\n");
        return 1;
    }
    const QString payload = args.at(1);
    const QString appDir = args.at(2);
    const QString exeName = args.at(3);
    const QString backup = appDir + QStringLiteral(".old");

    std::fprintf(stdout, "[apply-update] payload=%s\n", qPrintable(payload));
    std::fprintf(stdout, "[apply-update] appDir=%s\n", qPrintable(appDir));

    if (!QDir(payload).exists()) {
        std::fprintf(stderr, "[apply-update] payload 目录不存在\n");
        return 1;
    }

    // 清理上次残留的备份。
    if (QDir(backup).exists()) QDir(backup).removeRecursively();

    // 旧目录 → .old。Windows 上父进程可能尚在退出中（exe 句柄未释放），带重试。
    if (!renameDirWithRetry(appDir, backup, 60, 500)) {
        std::fprintf(stderr, "[apply-update] 无法移走旧目录（被占用？）：%s\n", qPrintable(appDir));
        return 1;
    }
    // payload → appDir。
    if (!renameDirWithRetry(payload, appDir, 10, 300)) {
        std::fprintf(stderr, "[apply-update] 无法就位新目录，回滚 .old\n");
        renameDirWithRetry(backup, appDir, 10, 300);
        return 1;
    }

    // 启动新版本。
    QString target = appDir + QLatin1Char('/') + exeName;
#ifdef Q_OS_MAC
    if (appDir.endsWith(QLatin1String(".app"))) {
        QString inner = exeName;
        if (inner.endsWith(QLatin1String(".exe"))) inner.chop(4);
        target = appDir + QStringLiteral("/Contents/MacOS/") + inner;
    }
#endif
    QProcess nx;
    nx.setProgram(target);
    nx.setArguments(QStringList{QStringLiteral("--updated-from"),
                                QFileInfo(backup).completeBaseName()});
    if (!nx.startDetached()) {
        std::fprintf(stderr, "[apply-update] 新版本启动失败：%s（回滚）\n", qPrintable(target));
        QDir(appDir).removeRecursively();
        renameDirWithRetry(backup, appDir, 10, 300);
        return 1;
    }
    std::fprintf(stdout, "[apply-update] 完成，新版本已启动。\n");
    return 0;
}

void cleanupUpdateResidue(const QString& appDir, const QString& dataDir) {
    const QString backup = appDir + QStringLiteral(".old");
    if (QDir(backup).exists()) {
        bool ok = false;
        for (int i = 0; i < 5 && !ok; ++i) {
            ok = QDir(backup).removeRecursively();
            if (!ok) {
#ifdef Q_OS_WIN
                ::Sleep(1000);
#else
                struct timespec ts{1, 0};
                nanosleep(&ts, nullptr);
#endif
            }
        }
        if (!ok) {
            std::fprintf(stderr, "[update] 旧版本残留目录清理失败（下次启动再试）：%s\n",
                         qPrintable(backup));
        } else {
            std::fprintf(stdout, "[update] 已清理旧版本残留目录。\n");
        }
    }
    // Windows updater 副本：上次 apply 用过的自身拷贝。
    const QString helper = dataDir + QStringLiteral("/updates/updater.exe");
    if (QFile::exists(helper)) QFile::remove(helper);
}

bool detectInstallerMode(const QString& appDir) {
    return QFile::exists(appDir + QStringLiteral("/.seimi-install-manifest"));
}

} // namespace seimi
