#pragma once

#include <QByteArray>
#include <QMutex>
#include <QMutexLocker>
#include <QString>

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

class QWebEngineProfile;

namespace seimi {

// 浏览器登录态（cookies）同步、注入与加密持久化中枢。
// HTTP 线程 record()，GUI 线程 applyTo()，跨线程经 QMutex。
// cookie 加密落盘 data/cookies.dat（SHA-256 密钥流 XOR + HMAC-SHA256），
// 密钥流由编译期 pepper + 机器绑定盐（data/seimi.key）PBKDF2 派生。
class CookieStore {
public:
    // 单条 cookie 规范表示（对应 Chrome cookies.getAll()）。
    struct Cookie {
        std::string name;        // 必填
        std::string value;       // 必填
        std::string domain;      // hostOnly=true 时用作 origin host，不设 setDomain
        std::string path = "/";  // 默认 "/"
        bool hostOnly = false;   // 精确 host 匹配（不含子域）
        bool secure = false;
        bool httpOnly = false;
        qint64 expirationDate = 0;  // epoch 秒；0 = session cookie
    };

    // dataDir 为持久化目录（二进制同级 data/）；空则禁用持久化（纯内存）。
    explicit CookieStore(const QString& dataDir = QString());
    ~CookieStore();

    CookieStore(const CookieStore&) = delete;
    CookieStore& operator=(const CookieStore&) = delete;

    // HTTP 线程：批量存入 cookie，返回入库数。注入由 GUI 线程异步完成。
    int record(const std::vector<Cookie>& cookies);

    // GUI 线程：把待注入 cookie 写进 profile->cookieStore()，清空缓冲。
    // profile 为 nullptr 时跳过。同时处理 pendingClear/pendingPurge 并 flush 落盘。
    void applyTo(QWebEngineProfile* profile);

    // 任意线程：标记清空，下次 applyTo 时 deleteAllCookies + 清概览 + 落空文件。
    void requestClear();

    // 任意线程：永久删除——删除 data/cookies.dat + 清全部内存状态。
    void requestPurge();

    // 任意线程：仅清概览计数（不触发 deleteAllCookies）。
    void clearOverview();

    // GUI 线程：强制把 dirty 仓库落盘（aboutToQuit 时调用）。
    void flush();

    // ====== 概览快照（GET /cookies）======

    struct DomainCount {
        std::string domain;
        int count = 0;
    };

    struct Overview {
        int total = 0;   // 累计注入过的 cookie 总数（去重后的当前计数）
        std::vector<DomainCount> domains;  // 按数量倒序
    };

    // 任意线程：取概览。maxDomains 限制返回条数（默认 200）。
    Overview snapshot(int maxDomains = 200) const;

private:
    // 把单条 Cookie 注入 profile（GUI 线程内）。
    static void injectOne(QWebEngineProfile* profile, const Cookie& c);

    // —— 加密持久化（均 GUI 线程或构造期调用）——
    void loadFromDisk();           // 构造时恢复
    void saveToDisk();             // 把 m_store 序列化 + 加密 + 写文件
    void deletePersistedFile();    // 删除 cookies.dat
    // cookie 全量 JSON 序列化/反序列化（明文中间态）。
    QByteArray serializeStore() const;
    void parseStore(const QByteArray& json);
    // 加密：AES-256-CBC + HMAC-SHA256(Encrypt-then-MAC)。失败返回空。
    QByteArray encryptData(const QByteArray& plain) const;
    QByteArray decryptData(const QByteArray& cipher) const;
    // 密钥派生：PBKDF2(pepper, 机器绑定盐, 100000, 32)。
    QByteArray deriveKey() const;
    void ensureSalt();   // 确保机器绑定盐存在（首次生成 data/seimi.key）

    // 生成持久仓库的 key（name + domain + path 去重）。
    static std::string storeKey(const Cookie& c);

    mutable QMutex m_mutex;
    std::vector<Cookie> m_pending;  // 待注入缓冲（applyTo 后清空）
    bool m_pendingClear = false;    // DELETE /cookies 标记
    bool m_pendingPurge = false;    // DELETE /cookies?permanent=1 标记
    // 概览：domain -> 当前数量。
    std::unordered_map<std::string, int> m_overview;
    int m_total = 0;

    // —— 持久化状态 ——
    QString m_dataDir;                              // data 目录（空=禁用持久化）
    std::map<std::string, Cookie> m_store;          // 全量 cookie 仓库（含 value，去重）
    bool m_dirty = false;                           // 待落盘标记（debounce）
};

} // namespace seimi
