// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

#include <cstdint>

class QNetworkAccessManager;
class QNetworkReply;
class QFile;
class QCryptographicHash;

namespace seimi {

// 更新管理器：检查 → 下载 → sha256 校验 → 应用。
//
// 状态机：Idle → Checking → {UpToDate | Available} → Downloading → Verifying → Ready
//         → Applying（本进程退出，交接给安装器/apply 子进程）
// 失败语义（对齐桌面端更新器的常见教训）：下载/校验失败退回 Available（失败 ≠ 跳过，
// 入口保留可重试）；取消下载同样退回 Available。仅检查失败进 Error。
//
// 应用路径按形态分叉：
//   - 安装版（appDir 存在 .seimi-install-manifest 标记，NSIS 安装时写入）：
//     静默重装——退出前 startDetached(installer, {"/S"})。
//   - 绿色版（默认）：解包到 data/updates/，spawn「--apply-update <payload> <appDir>
//     <exeName>」子进程做目录交换（旧目录 rename .old → payload 上位 → 启动新二进制），
//     新进程启动时清理 .old。任一步失败子进程回滚 .old。
//     Windows 上运行中的 exe 目录不可 rename——父进程先把自己复制到
//     data/updates/updater.exe 再以副本启动 apply（副本不在 appDir 内，可换目录）。
//
// 线程模型：全部 GUI 线程（QNAM 异步、QProcess 阻塞调用仅发生在终态交接前的短暂步骤，
// 解包用独立 QProcess + waitFinished，不碰事件循环里的渲染路径）。
class UpdateManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
    Q_PROPERTY(QString buildTime READ buildTime CONSTANT)
    Q_PROPERTY(QString gitCommit READ gitCommit CONSTANT)
    Q_PROPERTY(QString availableVersion READ availableVersion NOTIFY stateChanged)
    Q_PROPERTY(QString notesZh READ notesZh NOTIFY stateChanged)
    Q_PROPERTY(QString notesEn READ notesEn NOTIFY stateChanged)
    Q_PROPERTY(QString packageSizeText READ packageSizeText NOTIFY stateChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString progressText READ progressText NOTIFY progressChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged)
    Q_PROPERTY(bool installerMode READ installerMode CONSTANT)
    Q_PROPERTY(QString manifestUrl READ manifestUrl WRITE setManifestUrl NOTIFY manifestUrlChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)

public:
    enum class State {
        Idle, Checking, UpToDate, Available,
        Downloading, Verifying, Ready, Applying, Error
    };
    Q_ENUM(State)

    explicit UpdateManager(QObject* parent = nullptr);
    ~UpdateManager() override;

    // GUI 线程装配：dataDir = 持久化目录（下载落在 dataDir/updates/）。
    void init(const QString& dataDir);
    void maybeAutoCheck(bool enabled); // auto_check 开启且配了 manifestUrl 时延迟检查

    Q_INVOKABLE void checkNow();
    Q_INVOKABLE void startDownload();
    Q_INVOKABLE void cancelDownload();
    Q_INVOKABLE void applyAndRestart(); // Ready 态：交接并请求退出

    // ---- 属性 ----
    QString state() const;              // "idle"|"checking"|"up-to-date"|"available"|
                                        // "downloading"|"verifying"|"ready"|"applying"|"error"
    QString currentVersion() const { return m_currentVersion; }
    QString buildTime() const { return m_buildTime; }
    QString gitCommit() const { return m_gitCommit; }
    QString availableVersion() const { return m_availableVersion; }
    QString notesZh() const { return m_notesZh; }
    QString notesEn() const { return m_notesEn; }
    QString packageSizeText() const;
    double progress() const { return m_progress; }
    QString progressText() const { return m_progressText; }
    QString errorMessage() const { return m_errorMessage; }
    bool installerMode() const { return m_installerMode; }
    QString manifestUrl() const { return m_manifestUrl; }
    void setManifestUrl(const QString& url);
    bool busy() const;

signals:
    void stateChanged();
    void progressChanged();
    void manifestUrlChanged();
    // 应用阶段交接完成，主进程应当退出（main 连接到 QApplication::quit）。
    void requestQuit();

private:
    void setState(State s, const QString& error = QString());
    void onManifestReceived();
    void startAssetDownload();
    void onDownloadReadyRead();
    void onDownloadFinished();
    void onDownloadProgress(qint64 received, qint64 total);
    void finalizeAsset();          // 校验 + 进 Ready / 退回 Available
    bool extractArchive();         // tar -xf（绿色版）
    bool spawnApplyChild();        // 安装器静默重装 或 --apply-update 子进程
    void resetAssetState();

    QString m_dataDir;
    QString m_currentVersion = QStringLiteral("unknown");
    QString m_buildTime = QStringLiteral("unknown");
    QString m_gitCommit = QStringLiteral("unknown");
    bool m_installerMode = false;

    QString m_manifestUrl;
    State m_state = State::Idle;
    QString m_errorMessage;
    QString m_availableVersion;
    QString m_notesZh, m_notesEn;
    QString m_assetUrl;            // 选中的资产（installer 或 portable）
    QString m_assetSha256;
    QString m_assetKind;           // "installer" | "portable"
    qint64  m_assetSize = 0;       // manifest 声明或下载头给出；0 = 未知

    QNetworkAccessManager* m_nam = nullptr;
    QNetworkReply* m_reply = nullptr;
    QTimer m_checkTimeout;
    QFile* m_dlFile = nullptr;
    QCryptographicHash* m_hash = nullptr;
    qint64 m_received = 0;
    qint64 m_lastProgressMs = 0;
    qint64 m_lastProgressBytes = 0;
    QString m_verifiedPath;        // 校验通过的安装包/压缩包路径（Ready 态有效）
    double m_progress = 0.0;
    QString m_progressText;
};

// ---- --apply-update 子进程入口（main.cpp 在 QApplication 之前调用）----
// args: {stagedPayloadPath, appDirPath, exeFileName}
// 返回进程退出码（0 = 成功并已启动新版本；1 = 失败已回滚）。
int runApplyUpdate(const QStringList& args);

// 启动期清理更新残留：<appDir>.old 旧目录、data/updates/updater-*.exe 副本。
// Windows 文件锁/杀软扫描可能让删除暂态失败——带重试，失败仅告警（下次启动再清）。
void cleanupUpdateResidue(const QString& appDir, const QString& dataDir);

// 安装版标记检测（NSIS 安装器写入 <appDir>/.seimi-install-manifest）。
bool detectInstallerMode(const QString& appDir);

} // namespace seimi
