// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#include "CookieStore.h"

#include <qaesencryption.h>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkCookie>
#include <QRandomGenerator>
#include <QUrl>
#include <QWebEngineCookieStore>
#include <QWebEngineProfile>

#include <algorithm>

namespace seimi {

// 固定 pepper：编译进二进制。和机器绑定盐一起参与密钥派生。
// 单独拿到 data/ 目录（cookies.dat + seimi.key）但没有本二进制，无法反解。
static const char* kPepper = "seimi-render::cookie-vault::v1::7f3a9c2e1b8d";

// 参数
static constexpr int kPbkdf2Iterations = 100000;
static constexpr int kSaltLen = 16;       // 机器绑定盐长度
static constexpr int kKeyLen = 32;        // AES-256 主密钥长度
static constexpr int kIvLen = 16;         // AES-CBC IV 长度
static constexpr int kHmacLen = 32;       // HMAC-SHA256 输出长度

// HMAC-SHA256（RFC 2104）。Qt 无内置 HMAC，手搓。
static QByteArray hmacSha256(const QByteArray& key, const QByteArray& message) {
    constexpr int blockSize = 64; // SHA-256 block size
    QByteArray k = key;
    if (k.size() > blockSize) k = QCryptographicHash::hash(k, QCryptographicHash::Sha256);
    if (k.size() < blockSize) k.append(QByteArray(blockSize - k.size(), 0));
    QByteArray oPad(blockSize, 0x5c), iPad(blockSize, 0x36);
    for (int i = 0; i < blockSize; ++i) { oPad[i] = char(oPad[i] ^ k[i]); iPad[i] = char(iPad[i] ^ k[i]); }
    QByteArray inner = QCryptographicHash::hash(iPad + message, QCryptographicHash::Sha256);
    return QCryptographicHash::hash(oPad + inner, QCryptographicHash::Sha256);
}

// PBKDF2-HMAC-SHA256（手搓，RFC 8018）。
static QByteArray pbkdf2(const QByteArray& password, const QByteArray& salt,
                         int iterations, int dkLen) {
    QByteArray dk;
    int blocks = (dkLen + kHmacLen - 1) / kHmacLen;
    for (int i = 1; i <= blocks; ++i) {
        QByteArray intBytes(4, 0);
        intBytes[0] = char((i >> 24) & 0xff);
        intBytes[1] = char((i >> 16) & 0xff);
        intBytes[2] = char((i >> 8) & 0xff);
        intBytes[3] = char(i & 0xff);
        QByteArray u = hmacSha256(password, salt + intBytes);
        QByteArray t = u;
        for (int j = 1; j < iterations; ++j) {
            u = hmacSha256(password, u);
            for (int k = 0; k < kHmacLen; ++k) t[k] = char(t[k] ^ u[k]);
        }
        dk += t;
    }
    return dk.left(dkLen);
}

// ============================================================
// 构造 / 析构
// ============================================================

CookieStore::CookieStore(const QString& dataDir)
    : m_dataDir(dataDir) {
    if (!m_dataDir.isEmpty()) {
        QDir().mkpath(m_dataDir);  // 不存在则自动创建
        ensureSalt();
        loadFromDisk();            // 恢复历史 cookie 到 m_store + m_pending + 概览
    }
}

CookieStore::~CookieStore() {
    // 兜底落盘（正常退出走 flush，这里防异常退出）
    if (!m_dataDir.isEmpty()) {
        QMutexLocker locker(&m_mutex);
        if (m_dirty) saveToDisk();
    }
}

// ============================================================
// 持久化仓库的 key（去重）
// ============================================================

std::string CookieStore::storeKey(const Cookie& c) {
    return c.name + "\x01" + c.domain + "\x01" + c.path;
}

// ============================================================
// record —— HTTP 线程
// ============================================================

int CookieStore::record(const std::vector<Cookie>& cookies) {
    QMutexLocker locker(&m_mutex);
    int added = 0;
    for (const auto& c : cookies) {
        if (c.name.empty()) continue;  // name 必填
        m_pending.push_back(c);
        ++added;
        // 持久仓库：name+domain+path 去重，新值覆盖旧值。
        const std::string sk = storeKey(c);
        const bool isNew = m_store.find(sk) == m_store.end();
        m_store[sk] = c;
        m_dirty = true;
        // 概览/总数仅在【新增】键时累加：浏览器插件重复同步同一批 cookie 是覆盖更新，
        // 不应让 m_total/m_overview 随每次同步持续膨胀，偏离去重真相 m_store.size()。
        if (isNew) {
            std::string key = c.domain.empty() ? std::string("(unknown)") : c.domain;
            m_overview[key] = m_overview.count(key) ? m_overview[key] + 1 : 1;
            ++m_total;
        }
    }
    return added;
}

// ============================================================
// applyTo —— GUI 线程
// ============================================================

void CookieStore::applyTo(QWebEngineProfile* profile) {
    // 1) 先处理永久删除（DELETE /cookies?permanent=1）
    {
        QMutexLocker locker(&m_mutex);
        if (m_pendingPurge) {
            m_pendingPurge = false;
            m_pending.clear();
            m_overview.clear();
            m_total = 0;
            m_store.clear();
            m_dirty = false;
            locker.unlock();
            if (profile) profile->cookieStore()->deleteAllCookies();
            deletePersistedFile();
            return;
        }
    }

    // 2) 处理普通清空（DELETE /cookies）：清内存 + 清持久仓库 + 落空文件
    bool doClear = false;
    {
        QMutexLocker locker(&m_mutex);
        if (m_pendingClear) {
            doClear = true;
            m_pendingClear = false;
            m_overview.clear();
            m_total = 0;
            m_pending.clear();
            m_store.clear();   // 清持久仓库
            m_dirty = true;    // 标记要落空文件
        }
    }
    if (doClear) {
        if (profile) profile->cookieStore()->deleteAllCookies();
        saveToDisk();          // 落空文件（cookies.dat 内容为加密后的空数组）
        return;
    }

    // 3) 正常注入：把 m_pending drain 出来注入
    std::vector<Cookie> batch;
    {
        QMutexLocker locker(&m_mutex);
        if (m_pending.empty()) {
            // 顺带 flush 落盘
            if (m_dirty) { m_dirty = false; saveToDisk(); }
            return;
        }
        batch.swap(m_pending);
    }
    for (const auto& c : batch) {
        injectOne(profile, c);
    }

    // 4) 注入后 flush
    QMutexLocker locker(&m_mutex);
    if (m_dirty) { m_dirty = false; saveToDisk(); }
}

// ============================================================
// 请求清空 / 永久删除 / flush
// ============================================================

void CookieStore::requestClear() {
    QMutexLocker locker(&m_mutex);
    m_pendingClear = true;
}

void CookieStore::requestPurge() {
    QMutexLocker locker(&m_mutex);
    m_pendingPurge = true;
}

void CookieStore::clearOverview() {
    QMutexLocker locker(&m_mutex);
    m_overview.clear();
    m_total = 0;
    m_pending.clear();
}

void CookieStore::flush() {
    QMutexLocker locker(&m_mutex);
    if (m_dirty) { m_dirty = false; saveToDisk(); }
}

// ============================================================
// 注入（GUI 线程内，单条）
// ============================================================

void CookieStore::injectOne(QWebEngineProfile* profile, const Cookie& c) {
    if (!profile) return;

    QNetworkCookie cookie(
        QByteArray::fromStdString(c.name),
        QByteArray::fromStdString(c.value));
    cookie.setPath(QString::fromStdString(c.path.empty() ? "/" : c.path));
    cookie.setSecure(c.secure);
    cookie.setHttpOnly(c.httpOnly);

    if (c.expirationDate > 0) {
        cookie.setExpirationDate(
            QDateTime::fromSecsSinceEpoch(c.expirationDate));
    }

    QUrl origin;
    if (c.hostOnly) {
        QString scheme = c.secure ? QStringLiteral("https") : QStringLiteral("http");
        origin = QUrl(scheme + "://" + QString::fromStdString(c.domain) + "/");
    } else {
        cookie.setDomain(QString::fromStdString(c.domain));
        QString host = QString::fromStdString(c.domain);
        if (host.startsWith(QLatin1Char('.'))) host = host.mid(1);
        QString scheme = c.secure ? QStringLiteral("https") : QStringLiteral("http");
        origin = QUrl(scheme + "://" + host + "/");
    }

    profile->cookieStore()->setCookie(cookie, origin);
}

// ============================================================
// 概览快照
// ============================================================

CookieStore::Overview CookieStore::snapshot(int maxDomains) const {
    QMutexLocker locker(&m_mutex);
    Overview o;
    o.total = m_total;
    std::vector<DomainCount> all;
    all.reserve(m_overview.size());
    for (const auto& kv : m_overview) {
        all.push_back({kv.first, kv.second});
    }
    std::sort(all.begin(), all.end(),
              [](const DomainCount& a, const DomainCount& b) { return a.count > b.count; });
    if (maxDomains > 0 && int(all.size()) > maxDomains) all.resize(maxDomains);
    o.domains = std::move(all);
    return o;
}

// ============================================================
// ====== 加密持久化 ======
// ============================================================

void CookieStore::ensureSalt() {
    QFile f(m_dataDir + "/seimi.key");
    if (f.exists() && f.size() >= kSaltLen) return; // 已有
    // 用系统 CSPRNG 生成 16 字节随机盐（Mersenne Twister 等可预测 RNG 会让机器绑定盐失效）。
    QByteArray salt;
    QRandomGenerator* gen = QRandomGenerator::system();
    for (int i = 0; i < kSaltLen; ++i) salt.append(char(gen->generate() & 0xff));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(salt);
        f.close();
        f.setPermissions(QFile::ReadOwner | QFile::WriteOwner);  // 0600
    }
}

QByteArray CookieStore::deriveKey() const {
    QFile f(m_dataDir + "/seimi.key");
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    QByteArray salt = f.read(kSaltLen);
    f.close();
    if (salt.size() < kSaltLen) return QByteArray();
    return pbkdf2(QByteArray(kPepper), salt, kPbkdf2Iterations, kKeyLen);
}

QByteArray CookieStore::encryptData(const QByteArray& plain) const {
    QByteArray key = deriveKey();
    if (key.size() != kKeyLen) return QByteArray();
    // 加密密钥与 MAC 密钥分离派生（避免密钥复用）
    QByteArray encKey = pbkdf2(key, "enc", 1, kKeyLen);
    // 随机 IV 用系统 CSPRNG（CBC 模式下 IV 可预测会削弱语义安全）。
    QByteArray iv;
    QRandomGenerator* gen = QRandomGenerator::system();
    for (int i = 0; i < kIvLen; ++i) iv.append(char(gen->generate() & 0xff));
    // AES-256-CBC + PKCS7
    QAESEncryption aes(QAESEncryption::AES_256, QAESEncryption::CBC,
                       QAESEncryption::PKCS7);
    QByteArray cipher = aes.encode(plain, encKey, iv);
    // Encrypt-then-MAC: HMAC over (iv + cipher)
    QByteArray macKey = pbkdf2(key, "mac", 1, kKeyLen);
    QByteArray mac = hmacSha256(macKey, iv + cipher);
    // 文件格式: [magic 4B "SRCP"][iv 16B][mac 32B][cipher...]
    QByteArray out = "SRCP";
    out += iv + mac + cipher;
    return out;
}

QByteArray CookieStore::decryptData(const QByteArray& data) const {
    if (data.size() < 4 + kIvLen + kHmacLen) return QByteArray(); // 格式不全
    if (data.left(4) != "SRCP") return QByteArray();              // magic 不对
    QByteArray iv = data.mid(4, kIvLen);
    QByteArray mac = data.mid(4 + kIvLen, kHmacLen);
    QByteArray cipher = data.mid(4 + kIvLen + kHmacLen);

    // 先验 MAC（防篡改）
    QByteArray key = deriveKey();
    if (key.size() != kKeyLen) return QByteArray();
    QByteArray macKey = pbkdf2(key, "mac", 1, kKeyLen);
    QByteArray expectMac = hmacSha256(macKey, iv + cipher);
    if (mac != expectMac) return QByteArray(); // 篡改或密钥不匹配 → 视为无持久化

    QByteArray encKey = pbkdf2(key, "enc", 1, kKeyLen);
    QAESEncryption aes(QAESEncryption::AES_256, QAESEncryption::CBC,
                       QAESEncryption::PKCS7);
    return aes.decode(cipher, encKey, iv);
}

QByteArray CookieStore::serializeStore() const {
    // 序列化为明文 JSON 数组（中间态，之后整体加密）
    QJsonArray arr;
    for (const auto& kv : m_store) {
        const Cookie& c = kv.second;
        QJsonObject o;
        o["name"] = QString::fromStdString(c.name);
        o["value"] = QString::fromStdString(c.value);
        o["domain"] = QString::fromStdString(c.domain);
        o["path"] = QString::fromStdString(c.path);
        o["hostOnly"] = c.hostOnly;
        o["secure"] = c.secure;
        o["httpOnly"] = c.httpOnly;
        o["expirationDate"] = qint64(c.expirationDate);
        arr.append(o);
    }
    QJsonDocument doc(arr);
    return doc.toJson(QJsonDocument::Compact);
}

void CookieStore::parseStore(const QByteArray& json) {
    // 解析明文 JSON，填充 m_store + m_pending（让首次 applyTo 注入）+ 重建概览
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) return;
    for (const QJsonValue& v : doc.array()) {
        if (!v.isObject()) continue;
        QJsonObject o = v.toObject();
        Cookie c;
        c.name = o.value("name").toString().toStdString();
        if (c.name.empty()) continue;
        c.value = o.value("value").toString().toStdString();
        c.domain = o.value("domain").toString().toStdString();
        c.path = o.value("path").toString().toStdString();
        if (c.path.empty()) c.path = "/";
        c.hostOnly = o.value("hostOnly").toBool(false);
        c.secure = o.value("secure").toBool(false);
        c.httpOnly = o.value("httpOnly").toBool(false);
        c.expirationDate = qint64(o.value("expirationDate").toDouble(0.0));
        m_store[storeKey(c)] = c;
        m_pending.push_back(c);  // 让首次 applyTo 注入到 WebEngine
        // 概览
        std::string dk = c.domain.empty() ? std::string("(unknown)") : c.domain;
        m_overview[dk] = m_overview.count(dk) ? m_overview[dk] + 1 : 1;
        ++m_total;
    }
}

void CookieStore::saveToDisk() {
    // 调用者持锁。把 m_store 序列化 + 加密 + 写文件。
    if (m_dataDir.isEmpty()) return;
    QByteArray plain = serializeStore();
    QByteArray cipher = encryptData(plain);
    if (cipher.isEmpty()) return; // 加密失败（无盐/无密钥）静默跳过
    QFile f(m_dataDir + "/cookies.dat");
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(cipher);
        f.close();
        f.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    }
}

void CookieStore::loadFromDisk() {
    // 构造期调用（单线程）。读文件 + 解密 + 解析。
    if (m_dataDir.isEmpty()) return;
    QFile f(m_dataDir + "/cookies.dat");
    if (!f.exists()) return;            // 首次运行，无文件
    if (!f.open(QIODevice::ReadOnly)) return;
    QByteArray cipher = f.readAll();
    f.close();
    if (cipher.isEmpty()) return;
    QByteArray plain = decryptData(cipher);
    if (plain.isEmpty()) return;        // 解密失败（篡改/盐不匹配）→ 安全降级：视为无持久化
    parseStore(plain);
}

void CookieStore::deletePersistedFile() {
    if (m_dataDir.isEmpty()) return;
    QFile::remove(m_dataDir + "/cookies.dat");
}

} // namespace seimi
