# 百度搜索结果结构化提取 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 MCP `web_search engine=baidu` 返回去广告、结构化的搜索结果（标题/真实URL/摘要/来源），而非混着广告的整页 markdown。

**Architecture:** 新增第三条 JS 提取路径 `baidu_serp.js`（与 `extract.js`/`simplify.js` 同构：磁盘加载→DOM 注入→chunk 回读）。数据流 `RenderResult.serpJson → RenderTask.serpJson → Snapshot.serpJson → jsonStateResp` 三段串联到达 HTTP 响应；MCP `web_search` 消费 `serp_json` 组装结构化输出。`extract` 是与 `md_algorithm` 正交的新参数。

**Tech Stack:** C++17 / Qt6 WebEngine / cpp-httplib / cpp-mcp / 原生 JS（浏览器 DOM）。

**对应设计文档：** `docs/superpowers/specs/2026-07-01-baidu-serp-extraction-design.md`（已用 seimi-render 真实渲染校准过选择器）。

**测试方式：** 用真实渲染的百度页端到端验证（项目无 C++ 单测框架，符合现有 `scripts/smoke_test.sh` / `regression_test.py` 的真服务端到端测试惯例）。验证用 curl/PowerShell 直发请求，不入库。

**关键事实（实现时必读）：**
- 任务数据三段结构体：`RenderTask`（线程安全任务对象，`RenderTask.h`）/ `RenderQueue::RenderResult`（渲染产出，`RenderQueue.h:52-60`）/ `RenderQueue::Snapshot`（HTTP 读取快照，`RenderQueue.h:65-83`）。`serpJson` 必须三段都加，否则到不了 HTTP 响应。
- `submit` 有两个调用点：`HttpServer.cpp:824`、`WsServer.cpp:203`。加带默认值的尾部参数，两处零改动。
- settle 分支是 `RenderPool.cpp:349-360` 的内联 `QTimer::singleShot` lambda（不是名为 `onSettleElapsed` 的方法——`onSettleElapsed` 是更外层的 slot，调 `activateLazyImagesAndScroll`）。
- `jsonStateResp`（`HttpServer.cpp:222-266`）用裸字符串拼接 + `esc()`，`serp_json` 要裸嵌入（不过 esc，因为值本身是合法 JSON）。
- 本任务用打包发行版二进制测试：`build\dist\seimi-render-win-x64\seimi-render.exe --http-port 8088 --ws-port 8089`。

---

## 文件结构

| 文件 | 责任 | 改动 |
|---|---|---|
| `third_party/serp/baidu_serp.js` | 百度 SERP DOM 提取脚本（IIFE，chunk 协议） | 新增 |
| `src/RenderTask.h` | 定义 `ExtractAlgorithm` 枚举 + `RenderTask::extractAlgorithm`/`serpJson` | 改 |
| `src/RenderQueue.h` | `RenderResult::serpJson` + `Snapshot::serpJson` + `submit` 签名 | 改 |
| `src/RenderQueue.cpp` | `submit` 传参 + `reportSucceeded`/`snapshot` 拷 `serpJson` | 改 |
| `src/RenderPool.h` | `extractSerp`/`readSerpChunks` 声明 + `m_serpJsonResult`/`s_serpJs` 成员 | 改 |
| `src/RenderPool.cpp` | 第三条提取路径 + settle 分支 + `tryFinishCollect` 透传 | 改 |
| `src/HttpServer.cpp` | `parseExtractAlgorithm` + `/render` 解析 `extract` + `jsonStateResp` 裸嵌 `serp_json` | 改 |
| `src/McpServer.cpp` | `web_search` baidu 分支走 extract + 消费 serp_json 组装输出 | 改 |
| `CMakeLists.txt` | `third_party/serp/baidu_serp.js` install 规则（Task 6） | 改 |
| `AGENTS.md` | 文档补 `third_party/serp/` 与 SERP 提取机制（Task 11） | 改 |

**任务依赖顺序：** T1(枚举/字段) → T2(JS脚本) → T3(RenderQueue串联) → T4(RenderPool路径) → T5(HTTP参数/响应) → T6(CMake install，确保 baidu_serp.js 随包) → T7(构建+HTTP端到端验证) → T8(MCP编排) → T9(MCP端到端验证) → T10(WS回归) → T11(AGENTS.md 文档)。
T1-T5 是同一编译单元的递进改动，每步后构建一次确保不破编译。T6(CMake) 必须在 T7(端到端) 前完成，否则发行版没有 baidu_serp.js。

---

### Task 1: 定义 ExtractAlgorithm 枚举与 RenderTask 字段

**Files:**
- Modify: `src/RenderTask.h`（枚举加在 `MdAlgorithm` 后 `RenderTask` 前；字段加在结构体内）

- [ ] **Step 1: 在 `RenderTask.h` 的 `mdAlgorithmName` 函数后、`RenderTask` 结构体前，插入 `ExtractAlgorithm` 枚举**

在 `src/RenderTask.h`，定位到第 67 行（`mdAlgorithmName` 函数结束的 `}` 后的空行）。在它和第 69 行注释之间插入：

```cpp

// 站点特定结构化提取算法（与 MdAlgorithm 正交：MdAlgorithm 管 HTML→markdown 转换策略，
// ExtractAlgorithm 管"识别页面类型并结构化抽取"）。仅 extract != None 时有意义。
enum class ExtractAlgorithm : std::uint8_t {
    None,        // 默认：不做 SERP 提取（现有行为不变）
    BaiduSerp,   // 百度搜索结果页结构化提取（去广告，输出 results JSON）
    // 预留：GoogleSerp / BingSerp / DuckDuckGoSerp —— 框架就位后按需添加
};

inline const char* extractAlgorithmName(ExtractAlgorithm a) {
    switch (a) {
        case ExtractAlgorithm::None:      return "none";
        case ExtractAlgorithm::BaiduSerp: return "baidu_serp";
    }
    return "unknown";
}
```

- [ ] **Step 2: 给 `RenderTask` 加两个成员字段**

在 `src/RenderTask.h` 的 `RenderTask` 结构体内：

(a) 在第 80 行 `const MdAlgorithm mdAlgorithm;` 后加一行不可变字段：

```cpp
    const ExtractAlgorithm extractAlgorithm;  // 站点特定提取算法（默认 None，不影响现有行为）
```

(b) 在第 91 行 `MdAlgorithm mdAlgorithmUsed{MdAlgorithm::Conservative};` 后、第 92 行 `QString error;` 前加一个可变字段：

```cpp
    QString serpJson;                  // SERP 结构化提取结果 JSON 字符串（extract!=None 且成功时填）
```

- [ ] **Step 3: 扩展 `RenderTask` 构造函数**

在 `src/RenderTask.h`，把第 99-107 行的构造函数改为（加 `extractAlg` 参数，带默认值 `None`）：

```cpp
    RenderTask(QString id_, QString url_, qint64 createdAt, int settleDelay, OutputMask outs,
               ImageFormat imgFmt = ImageFormat::Auto, MdAlgorithm mdAlg = MdAlgorithm::Conservative,
               ExtractAlgorithm extractAlg = ExtractAlgorithm::None)
        : id(std::move(id_))
        , url(std::move(url_))
        , createdAtMsec(createdAt)
        , settleDelayMsec(settleDelay)
        , outputs(outs)
        , imageFormat(imgFmt)
        , mdAlgorithm(mdAlg)
        , extractAlgorithm(extractAlg) {}
```

- [ ] **Step 4: 构建确认编译通过**

Run: `cmake --build build --target seimi-render 2>&1 | findstr /i "error warning RenderTask"`
Expected: 无 error（可能有其它无关 warning）。`extractAlgorithm` 默认 `None`，现有所有 `RenderTask::create(...)` 调用零改动。

---

### Task 2: 编写 baidu_serp.js 提取脚本

**Files:**
- Create: `third_party/serp/baidu_serp.js`

- [ ] **Step 1: 创建 `third_party/serp/` 目录与脚本文件**

创建 `third_party/serp/baidu_serp.js`，完整内容（已用真实渲染的百度 DOM 校准选择器，见设计文档第 1 节）：

```js
// seimi-render 百度搜索结果页（SERP）结构化提取脚本。
// 由 RenderWorker::extractSerp() 在 GUI 线程注入到已加载的百度结果页 DOM 上运行。
// 选择器基于 2026-07 真实渲染校准（见 docs/superpowers/specs/2026-07-01-baidu-serp-extraction-design.md）。
// 协议与 extract.js/simplify.js 同构：结果存 window.__seimiSerp.chunks（30k 分块），
// 返回值只含小状态 JSON {ok,totalChunks}，规避 runJavaScript 大返回值截断。
(function () {
  function extractBaiduSerp() {
    var out = { engine: "baidu", blocked: false, results: [], recommend: [], meta: {} };

    // 1. 反爬拦截页检测：title 含"安全验证"或 body 文本极短（实测拦截页仅 ~1.5KB）。
    var t = document.title || "";
    var bodyText = (document.body && document.body.innerText) || "";
    if (/安全验证/.test(t) || bodyText.length < 50) {
      out.blocked = true;
      return stash(out);
    }

    // 2. 限定主结果区（实测所有结果都在 #content_left 内）。
    var scope = document.querySelector('#content_left') || document.body;

    // 3. 收集所有 c-container 候选（自然结果 + 百度卡片 + 广告都在内）。
    var containers = scope.querySelectorAll('div.c-container');
    var seenTitles = {};  // 嵌套去重：广告 div 内嵌套同标题 div
    var adCount = 0;

    containers.forEach(function (c) {
      var cls = c.className || "";
      var mu = c.getAttribute("mu") || "";
      var tpl = c.getAttribute("tpl") || "";

      // 3a. 广告判定（实测可靠信号）：
      //   - 容器 class 含 EC_result / ec-pc-huanxin-grid / ec-pc-fresh-font-color
      //   - 子元素有 <span class="ec-tuiguang"> 角标
      //   实测：tuiguang 不在容器 class 而在子 span；整页无"广告"字面文本。
      var isAd = /EC_result|ec-pc-huanxin|ec-pc-fresh-font-color/.test(cls) ||
                 !!c.querySelector('span[class*="ec-tuiguang"]');
      if (isAd) { adCount++; return; }

      // 3b. "其他人还在搜"聚合剔除。
      if (tpl === "recommend_list") return;

      // 3c. 标题（容器内 h3 > a 或 h3）。
      var h3a = c.querySelector('h3 a') || c.querySelector('h3');
      if (!h3a) return;
      var title = (h3a.innerText || h3a.textContent || "").trim();
      if (!title || seenTitles[title]) return;  // 嵌套去重
      seenTitles[title] = true;

      // 3d. URL：mu 优先（实测自然结果 mu=真实URL，如 python.org）。
      //     mu 缺失或为 baidu.com/link?/nourl.ubs → 跳转链，标 is_redirect。
      var href = (h3a.getAttribute && h3a.getAttribute("href")) || "";
      var url = mu || href;
      var isRedirect = !mu || /baidu\.com\/link\?url=/.test(url) || /nourl\.ubs\.baidu\.com/.test(url);

      // 3e. 摘要（实测 summary-text_560AW，class 末尾常带换行空格，用前缀通配）。
      var desc = "";
      var descEl = c.querySelector('span[class*="summary-text"], .c-abstract, span[class*="content-right"]');
      if (descEl) desc = (descEl.innerText || "").trim();

      // 3f. 来源（实测 cosc-source-text 精确命中）。
      var source = "";
      var srcEl = c.querySelector('.cosc-source-text, .c-showurl');
      if (srcEl) source = (srcEl.innerText || "").trim();

      // 3g. 类型：result-op 为百度聚合卡（百科/翻译/贴吧等）。
      var isCard = /result-op/.test(cls);

      out.results.push({
        title: title,
        url: url,
        is_redirect: isRedirect,
        snippet: desc,
        source: source,
        type: isCard ? "baidu_card" : "organic"
      });
    });

    // 4. 相关搜索（#rs 区，单独入 recommend，不混入 results）。
    var rs = document.querySelector('#rs');
    if (rs) rs.querySelectorAll('a').forEach(function (a) {
      var tx = (a.innerText || "").trim();
      if (tx) out.recommend.push(tx);
    });

    out.meta = { count: out.results.length, ads_filtered: adCount };
    return stash(out);
  }

  // 把结果对象分块存入 window，返回小状态 JSON（与 extract.js/simplify.js 同协议）。
  function stash(obj) {
    var s = JSON.stringify(obj);
    var CHUNK = 30000, chunks = [];
    for (var i = 0; i < s.length; i += CHUNK) chunks.push(s.substring(i, i + CHUNK));
    window.__seimiSerp = { chunks: chunks };
    return JSON.stringify({ ok: true, totalChunks: chunks.length });
  }

  try {
    return extractBaiduSerp();
  } catch (e) {
    window.__seimiSerp = { chunks: [""] };
    return JSON.stringify({ ok: false, error: "" + e });
  }
})();
```

- [ ] **Step 2: 确认文件就位**

Run: `dir third_party\serp\baidu_serp.js`
Expected: 显示文件，非空。

---

### Task 3: RenderQueue 三段结构体串联 serpJson + submit 签名

**Files:**
- Modify: `src/RenderQueue.h`（`RenderResult` / `Snapshot` / `submit`）
- Modify: `src/RenderQueue.cpp`（`submit` / `reportSucceeded` / `snapshot`）

- [ ] **Step 1: `RenderResult` 加 `serpJson` 字段**

在 `src/RenderQueue.h` 第 52-60 行的 `struct RenderResult` 内，在 `MdAlgorithm mdAlgorithmUsed...`（第 59 行）后加：

```cpp
        QString serpJson;                   // SERP 结构化提取结果 JSON（extract!=None 且成功时填）
```

- [ ] **Step 2: `Snapshot` 加 `serpJson` 字段**

在 `src/RenderQueue.h` 第 65-83 行的 `struct Snapshot` 内，在 `MdAlgorithm mdAlgorithmUsed...`（第 78 行）后加：

```cpp
        QString serpJson;                   // SERP 结构化提取结果 JSON（成功时）
```

- [ ] **Step 3: `submit` 签名加 `extractAlg` 参数**

在 `src/RenderQueue.h` 第 43-44 行，把声明改为：

```cpp
    QString submit(QString url, int settleDelayMsec, OutputMask outputs,
                   ImageFormat imageFormat, MdAlgorithm mdAlgorithm, qint64 nowMsec,
                   ExtractAlgorithm extractAlgorithm = ExtractAlgorithm::None);
```

- [ ] **Step 4: `submit` 实现传参**

在 `src/RenderQueue.cpp` 第 29-30 行，把函数签名改为与声明一致（加最后一个参数）：

```cpp
QString RenderQueue::submit(QString url, int settleDelayMsec, OutputMask outputs,
                            ImageFormat imageFormat, MdAlgorithm mdAlgorithm, qint64 nowMsec,
                            ExtractAlgorithm extractAlgorithm) {
```

然后在第 42-43 行，把 `RenderTaskPtr::create(...)` 调用改为传入新参数：

```cpp
    auto task = RenderTaskPtr::create(id, std::move(url), nowMsec, settleDelayMsec,
                                      outputs, imageFormat, mdAlgorithm, extractAlgorithm);
```

- [ ] **Step 5: `reportSucceeded` 把 result.serpJson 拷入 task**

在 `src/RenderQueue.cpp` 第 149-161 行的 `reportSucceeded`，在 lambda 内（第 158 行 `t.mdAlgorithmUsed = result.mdAlgorithmUsed;` 后）加一行：

```cpp
                    t.serpJson = result.serpJson;
```

- [ ] **Step 6: `snapshot` 把 task.serpJson 拷入快照**

在 `src/RenderQueue.cpp` 第 202-226 行的 `snapshot`，在第 220 行 `s.mdAlgorithmUsed = t.mdAlgorithmUsed;` 后加一行：

```cpp
    s.serpJson = t.serpJson;
```

- [ ] **Step 7: 构建确认编译通过**

Run: `cmake --build build --target seimi-render 2>&1 | findstr /i "error"`
Expected: 无 error。`submit` 新参数带默认值，两个调用点（HttpServer.cpp:824、WsServer.cpp:203）零改动。

---

### Task 4: RenderPool 第三条提取路径

**Files:**
- Modify: `src/RenderPool.h`（声明 `extractSerp`/`readSerpChunks` + 成员）
- Modify: `src/RenderPool.cpp`（实现 + settle 分支 + tryFinishCollect 透传）

- [ ] **Step 1: `RenderPool.h` 加方法声明与成员**

在 `src/RenderPool.h` 第 89 行（`readSimplifyChunks` 声明）后，加一组 SERP 提取声明：

```cpp

    // —— 站点特定 SERP 结构化提取（extract != None 时）——
    // extractAlgorithm==BaiduSerp 时，在 collect 前注入 baidu_serp.js，在 live DOM 上
    // 结构化抽取搜索结果（去广告），结果 JSON 存 m_serpJsonResult。tryFinishCollect 透传到
    // RenderResult.serpJson。失败（JS 异常/加载失败）则 m_serpJsonResult 留空，降级正常渲染。
    void extractSerp();
    // 分块读取 SERP 结果（与 readSimplifyChunks 同协议，CHUNK=30000）。
    void readSerpChunks(int idx, int total, const QString& acc);
```

在第 167 行（`QString m_simplifiedHtml;`）后加成员：

```cpp
    // SERP 提取结果 JSON（仅 extractAlgorithm==BaiduSerp 时填充）。
    // 空则表示未启用或提取失败，tryFinishCollect 不透传 serpJson（降级正常渲染）。
    QString m_serpJsonResult;
```

在第 175 行（`static QString s_simplifyJs;`）后加静态缓存：

```cpp
    // baidu_serp.js 内容缓存，首次使用时从磁盘加载，之后复用。
    static QString s_serpJs;
```

- [ ] **Step 2: `RenderPool.cpp` 定义静态缓存成员**

在 `src/RenderPool.cpp` 第 50 行（`QString RenderWorker::s_simplifyJs;`）后加：

```cpp
QString RenderWorker::s_serpJs;
```

- [ ] **Step 3: `RenderPool.cpp` 加 `extractSerp` 与 `readSerpChunks` 实现**

在 `src/RenderPool.cpp` 第 504 行（`readSimplifyChunks` 函数结束的 `}`）后、第 506 行 `collect()` 前，插入两个函数（结构与 `extractSimplifiedHtml`/`readSimplifyChunks` 几乎逐行对称）：

```cpp

// SERP 结构化提取：注入 baidu_serp.js，在 live DOM 上抽取搜索结果 JSON，
// 存入 m_serpJsonResult，完成后调 collect()。
// baidu_serp.js 首次使用时从可执行文件同级的 third_party/serp/baidu_serp.js 加载并缓存。
// 加载失败或 JS 执行失败时 m_serpJsonResult 留空，tryFinishCollect 不透传（降级正常渲染）。
void RenderWorker::extractSerp() {
    if (!m_busy || !m_task || !m_page) { collect(); return; }

    // 首次使用：加载 baidu_serp.js。查找路径优先可执行文件同级，其次源码目录（开发期）。
    if (s_serpJs.isEmpty()) {
        QStringList candidates = {
            QCoreApplication::applicationDirPath() + QStringLiteral("/third_party/serp/baidu_serp.js"),
#ifdef SEIMI_ADMIN_UI_SRC_DIR
            QStringLiteral(SEIMI_ADMIN_UI_SRC_DIR) + QStringLiteral("/../third_party/serp/baidu_serp.js"),
#endif
        };
        for (const QString& path : candidates) {
            QFileInfo fi(path);
            if (!fi.isFile()) continue;
            QFile f(path);
            if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                s_serpJs = QString::fromUtf8(f.readAll());
                break;
            }
        }
        if (s_serpJs.isEmpty()) {
            // baidu_serp.js 找不到：无法做 SERP 提取，直接采集（m_serpJsonResult 留空）。
            collect();
            return;
        }
    }

    // 注入 baidu_serp.js（IIFE）。JS 把结果分块存入 window.__seimiSerp，
    // 返回值只含状态 JSON（ok + totalChunks），不截断。C++ 逐块 runJavaScript 读取拼接。
    m_page->runJavaScript(s_serpJs, [this](const QVariant& v) {
        if (!m_busy || !m_task || !m_page) { collect(); return; }
        const QString s = v.toString();
        const QJsonDocument doc = QJsonDocument::fromJson(s.toUtf8());
        if (!doc.isObject()) { collect(); return; }
        QJsonObject o = doc.object();
        if (!o.value(QStringLiteral("ok")).toBool()) {
            // JS 异常：m_serpJsonResult 留空，降级正常渲染。
            collect();
            return;
        }
        int totalChunks = o.value(QStringLiteral("totalChunks")).toInt(0);
        if (totalChunks <= 0) { collect(); return; }
        readSerpChunks(0, totalChunks, QString());
    });
}

// 分块读取 SERP 结果（与 readSimplifyChunks 同协议，规避大返回值截断）。
void RenderWorker::readSerpChunks(int idx, int total, const QString& acc) {
    if (!m_busy || !m_task || !m_page) { collect(); return; }
    if (idx >= total) {
        m_serpJsonResult = acc;
        collect();
        return;
    }
    QString js = QStringLiteral(
        "(window.__seimiSerp && window.__seimiSerp.chunks ? "
        "window.__seimiSerp.chunks[%1] : null)").arg(idx);
    m_page->runJavaScript(js, [this, idx, total, acc](const QVariant& v) {
        if (!m_busy || !m_task || !m_page) { collect(); return; }
        if (v.isNull()) { collect(); return; }  // window 变量丢失，降级
        readSerpChunks(idx + 1, total, acc + v.toString());
    });
}
```

- [ ] **Step 4: settle lambda 加 SERP 分支**

在 `src/RenderPool.cpp` 第 349-360 行的 `QTimer::singleShot(2500, ...)` lambda 内，把分支改为三分支（SERP 优先于 markdown，因为 SERP 页不转 markdown）：

```cpp
        QTimer::singleShot(2500, this, [this]() {
            if (!m_busy || !m_task || !m_page) return;
            if (m_task->extractAlgorithm != ExtractAlgorithm::None) {
                extractSerp();              // SERP 结构化提取（与 markdown 互斥）
            } else if (m_task->wants(Output::Markdown)) {
                if (m_task->mdAlgorithm == MdAlgorithm::Readability) {
                    extractReadability();        // 正文定位（概率性，质量高）
                } else {
                    extractSimplifiedHtml();     // conservative 新实现：DOM 简化（默认）
                }
            } else {
                collect();
            }
        });
```

- [ ] **Step 5: `tryFinishCollect` 透传 serpJson**

在 `src/RenderPool.cpp` 第 615 行（markdown 转换的 `}` 后、第 617 行 `if (m_task->wants(Output::Pdf))` 前），插入 SERP 透传（result 是 `RenderQueue::RenderResult`，第 589 行定义）：

```cpp

    // SERP 结构化提取结果透传（仅 extract != None 且提取成功时）。
    if (m_task->extractAlgorithm != ExtractAlgorithm::None && !m_serpJsonResult.isEmpty()) {
        result.serpJson = m_serpJsonResult;
    }
```

- [ ] **Step 6: `destroyPage` 复位 m_serpJsonResult**

在 `src/RenderPool.cpp` 第 671 行（`destroyPage()` 内的 `m_simplifiedHtml.clear();`）后加一行：

```cpp
    m_serpJsonResult.clear();
```

（与 `m_readabilityHtml.clear()`/`m_simplifiedHtml.clear()` 并列，任务结束时复位，避免残留影响下个任务。）

- [ ] **Step 7: 构建确认编译通过**

Run: `cmake --build build --target seimi-render 2>&1 | findstr /i "error"`
Expected: 无 error。

---

### Task 5: HTTP /render 加 extract 参数与 serp_json 响应

**Files:**
- Modify: `src/HttpServer.cpp`（`parseExtractAlgorithm` + `/render` 解析 + `jsonStateResp` 嵌入）

- [ ] **Step 1: 加 `parseExtractAlgorithm` 解析函数**

在 `src/HttpServer.cpp` 第 68 行（`parseMdAlgorithm` 函数结束的 `}`）后加：

```cpp

// 解析站点特定提取算法：baidu_serp/baidu。默认 none（不影响现有行为）。
static ExtractAlgorithm parseExtractAlgorithm(const QJsonValue& v) {
    QString t = v.toString().trimmed().toLower();
    if (t == QStringLiteral("baidu_serp") || t == QStringLiteral("baidu"))
        return ExtractAlgorithm::BaiduSerp;
    return ExtractAlgorithm::None;
}
```

- [ ] **Step 2: `/render` handler 解析 extract 参数并传入 submit**

在 `src/HttpServer.cpp` 第 822 行（`MdAlgorithm mdAlg = parseMdAlgorithm(...)`）后加：

```cpp
        // 站点特定提取算法：baidu_serp（百度搜索结果页结构化提取）。默认 none。
        ExtractAlgorithm extractAlg = parseExtractAlgorithm(obj.value(QStringLiteral("extract")));
```

然后把第 824 行的 `submit` 调用改为传入 `extractAlg`：

```cpp
        QString id = m_queue->submit(url, settleMs, outputs, imgFmt, mdAlg, nowMsec(), extractAlg);
```

- [ ] **Step 3: `jsonStateResp` 裸嵌入 serp_json**

在 `src/HttpServer.cpp` 的 `jsonStateResp`（第 222-266 行），在第 263 行 `}`（Screenshot 分支结束）后、第 264 行 `out += "}";` 前，插入 serp_json 裸嵌入（注意：不过 esc，因为 serpJson 本身是合法 JSON 字符串；末尾无引号包裹）：

```cpp
        // SERP 结构化提取结果（裸嵌入：serpJson 本身是合法 JSON 对象字符串，
        // 不能过 esc() 否则破坏内部结构）。
        if (!s.serpJson.isEmpty()) {
            out += ",\"serp_json\":" + s.serpJson.toStdString();
        }
```

- [ ] **Step 4: 构建确认编译通过**

Run: `cmake --build build --target seimi-render 2>&1 | findstr /i "error"`
Expected: 无 error。

---

### Task 6: CMake install 规则（确保 baidu_serp.js 随发行包）

**Files:**
- Modify: `CMakeLists.txt`（install 规则，第 359 行后）

- [ ] **Step 1: CMake 加 baidu_serp.js install 规则**

在 `CMakeLists.txt` 第 359 行（stealth install 规则的 `DESTINATION third_party/stealth)`）后加：

```cmake
# 百度搜索结果结构化提取脚本（SERP）随二进制安装到同级 third_party/serp/
# 运行时从磁盘加载，由 RenderWorker::extractSerp 注入到百度结果页 DOM 提取去广告的结构化结果。
install(FILES third_party/serp/baidu_serp.js
        DESTINATION third_party/serp)
```

- [ ] **Step 2: 重新构建打包发行版**

Run: `scripts\build-windows.bat`
Expected: 构建成功，`build\dist\seimi-render-win-x64\third_party\serp\baidu_serp.js` 存在。

---

### Task 7: HTTP 端到端验证（真实百度渲染）

**Files:**
- Test: 手动 curl/PowerShell（不入库）

- [ ] **Step 1: 启动服务**

Run: `build\dist\seimi-render-win-x64\seimi-render.exe --http-port 8088 --ws-port 8089`
（后台运行，等 `/health` 返回 ok）

- [ ] **Step 2: 发起带 extract 的渲染请求并校验**

用 PowerShell（`Invoke-WebRequest`）或 curl 发：

```bash
POST http://127.0.0.1:8088/render
{"url":"https://www.baidu.com/s?wd=Python","output":"html","extract":"baidu_serp","settle_ms":4000,"long_poll_ms":50000}
```

Expected 响应：
- `state` = `succeeded`
- 含 `serp_json` 字段（裸 JSON 对象）
- `serp_json.blocked` = `false`
- `serp_json.results` 数组非空（应有 python.org / 百度百科 / 菜鸟教程等）
- `serp_json.results` 中**无** type 为广告的条目，且标题里**无** "Py编程软件"/"python-2026最新版" 这类被 `EC_result` 标记的广告
- `serp_json.meta.ads_filtered` >= 1
- 第一条 organic 结果 `url` = `https://www.python.org/`，`is_redirect` = `false`

- [ ] **Step 3: 若 serp_json 缺失，按降级路径排查**

若响应无 `serp_json` 字段：
1. 确认 `build\dist\seimi-render-win-x64\third_party\serp\baidu_serp.js` 存在（Task 6 install）。
2. 看 `html` 字段是否非空（渲染本身应成功）。
3. JS 提取失败会静默降级——临时在 `extractSerp` 的 `runJavaScript` 回调里 qWarning 打印返回值排查。

- [ ] **Step 4: 关闭服务**

停掉测试服务进程。

---

### Task 8: MCP web_search baidu 分支编排

**Files:**
- Modify: `src/McpServer.cpp`（`web_search` handler，第 469-541 行）

- [ ] **Step 1: 改造 web_search 的请求构造（baidu 走 extract 路径）**

在 `src/McpServer.cpp` 的 `web_search` handler（第 469 行起的 lambda），定位到第 491-499 行构造 `body` 的部分。把它替换为按引擎分流：

```cpp
            int settle = params.value("settle_ms", 2500);
            int timeout = params.value("timeout_ms", 45000);
            bool isBaidu = el.find("baidu") != std::string::npos;
            mcp::json body = {
                {"url", url},
                {"settle_ms", settle},
                {"long_poll_ms", timeout},
            };
            if (isBaidu) {
                // baidu：走结构化提取（去广告，输出 serp_json），不转 markdown（SERP 页 markdown 是噪声）。
                body["output"] = "html";
                body["extract"] = "baidu_serp";
            } else {
                // google/bing/ddg：维持现状（整页 conservative markdown）。
                body["output"] = "markdown";
                body["md_algorithm"] = "conservative";
            }
            std::string resp = impl->httpPost("/render", body.dump());
```

- [ ] **Step 2: 改造响应消费（baidu 消费 serp_json，非 baidu 维持 markdown）**

在 `src/McpServer.cpp`，定位到第 515 行 `if (state != "succeeded")` 分支之后、第 533 行 `std::string md = ...` 之前。把第 533-537 行（取 markdown 拼 text 的部分）替换为按引擎分流的完整逻辑：

```cpp
                // baidu：消费 serp_json 组装结构化输出。
                if (isBaidu) {
                    if (!j.contains("serp_json")) {
                        // 提取未生效（JS 缺失/异常）→ 降级返回 raw html 前 2000 字符诊断。
                        std::string htmlRaw = j.value("html", std::string());
                        std::string text = "[web_search engine=baidu query=\"" + query + "\"]\n"
                                           "(extraction unavailable; raw html preview)\n\n" +
                                           htmlRaw.substr(0, 2000);
                        return toTextContent(text);
                    }
                    auto sj = j["serp_json"];
                    if (sj.value("blocked", false)) {
                        return toTextContent(
                            "[web_search engine=baidu query=\"" + query + "\"]\n"
                            "BLOCKED: Baidu returned a security-verification page. "
                            "Suggestions:\n"
                            "1. Retry after a few seconds.\n"
                            "2. Try engine='bing'.\n"
                            "3. Increase timeout_ms (e.g. 60000).");
                    }
                    auto results = sj.value("results", mcp::json::array());
                    if (results.empty()) {
                        return toTextContent(
                            "[web_search engine=baidu query=\"" + query + "\"]\n"
                            "(no results found)");
                    }
                    // 组装：JSON 块（精确可解析）+ 可读编号列表。
                    std::string text = "[web_search engine=baidu query=\"" + query +
                                       "\" count=" + std::to_string(results.size()) + "]\n\n";
                    text += "```json\n" + results.dump(2) + "\n```\n\n";
                    text += "Results:\n";
                    int idx = 1;
                    for (auto& r : results) {
                        text += std::to_string(idx++) + ". **" + r.value("title", std::string()) + "**";
                        std::string src = r.value("source", std::string());
                        if (!src.empty()) text += " — " + src;
                        text += "\n   " + r.value("url", std::string());
                        if (r.value("is_redirect", false)) text += "  (baidu redirect link)";
                        std::string snip = r.value("snippet", std::string());
                        if (!snip.empty()) text += "\n   " + snip;
                        text += "\n";
                        if (r.value("type", std::string()) == "baidu_card")
                            text += "   _[baidu aggregated card]_\n";
                    }
                    auto rec = sj.value("recommend", mcp::json::array());
                    if (!rec.empty()) {
                        text += "\nRelated searches: ";
                        for (size_t i = 0; i < rec.size(); ++i)
                            text += (i ? ", " : "") + rec[i].get<std::string>();
                        text += "\n";
                    }
                    return toTextContent(text);
                }
                // 非 baidu：维持现有 markdown 返回。
                std::string md = j.value("markdown", std::string());
                std::string text = "[web_search engine=" + engine +
                                   " query=\"" + query + "\"]\n\n";
                text += md.empty() ? "(no results / empty page)" : md;
                return toTextContent(text);
```

- [ ] **Step 3: 更新 web_search 工具描述**

在 `src/McpServer.cpp` 第 435-449 行的 `with_description(...)`，把描述里关于 baidu 的说明更新（强调结构化）。把第 444-448 行的 NOTE 段替换为：

```cpp
            "NOTE: for engine='baidu', results are returned as a clean structured "
            "list (ads/promoted results filtered out, each result has title/url/"
            "snippet/source). For other engines the full result-page markdown is "
            "returned (may include ads). After reading the results, call web_reader "
            "(or render_url) on any result link to open the full article."
```

- [ ] **Step 4: 构建确认编译通过**

Run: `cmake --build build --target seimi-render 2>&1 | findstr /i "error"`
Expected: 无 error。

---

### Task 9: MCP 端到端验证

**Files:**
- Test: 手动 MCP 调用（不入库）

- [ ] **Step 1: 启动服务（含 MCP 端口）**

Run: `build\dist\seimi-render-win-x64\seimi-render.exe --http-port 8088 --ws-port 8089 --mcp-port 8090`

- [ ] **Step 2: 用 JSON-RPC 调 web_search engine=baidu**

用 curl/PowerShell 发 MCP 请求（Streamable HTTP，JSON-RPC over POST）。简化：先 initialize 再 tools/call：

```
POST http://127.0.0.1:8090/mcp
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}}}
POST http://127.0.0.1:8090/mcp
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"web_search","arguments":{"query":"Python","engine":"baidu"}}}
```

Expected：
- 返回 `content[0].text` 含 `[web_search engine=baidu query="Python" count=N]`
- 含 ```json ``` 块（results 数组）
- 含编号 `Results:` 列表，每条标题/url
- 无广告条目（"Py编程软件"等不应出现）

- [ ] **Step 3: 回归验证 google/bing 不受影响**

```
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"web_search","arguments":{"query":"Python","engine":"bing"}}}
```

Expected：返回 `[web_search engine=bing ...]` + markdown 内容（与改造前行为一致，无 serp_json/结构化）。

- [ ] **Step 4: 关闭服务**

---

### Task 10: WebSocket render 回归验证（确保 submit 签名扩展不破 WS）

**Files:**
- Test: 手动 WS 调用（不入库）

- [ ] **Step 1: 启动服务**

同 Task 9 Step 1。

- [ ] **Step 2: 用 WS 发 render 请求（不带 extract，应走原 markdown 路径）**

连 `ws://127.0.0.1:8089/`，发：

```json
{"action":"render","url":"https://www.baidu.com/s?wd=Python","output":"markdown"}
```

Expected：收到 `created` 事件，再收到 `finished` 事件带 `state:succeeded`。`submit` 新参数默认 None，WS 路径行为不变（返回 markdown，无 serp_json）。

- [ ] **Step 3: 关闭服务**

---

### Task 11: AGENTS.md 文档 + 最终提交

**Files:**
- Modify: `AGENTS.md`（模块索引 + 资源说明 + 改动者提醒）

- [ ] **Step 1: AGENTS.md 模块索引补 third_party/serp/**

在 `AGENTS.md` 的「目录速览」`third_party/` 条目，把已有列表里补上 `serp`。定位到含 `qaes`、`cpp-mcp` 的那一行（`- \`third_party/\` — vendored 依赖` 开头），在该行末尾的依赖列表里加 `baidu_serp.js`（百度 SERP 提取）。

- [ ] **Step 2: AGENTS.md 给改动者的提醒补 SERP 提取机制**

在 `AGENTS.md` 的「给改动者的提醒」章节，加一条（放在 stealth 提醒之后）：

```markdown
- 改 `baidu_serp.js` 时：百度 HTML 结构会随版本变（class 名带 hash 后缀），改时用 seimi-render 真实渲染百度页拿 DOM 校准（curl 直取只能拿到反爬拦截页）。脚本经 `RenderWorker::extractSerp` 注入，改后随二进制 install 到 `third_party/serp/`，运行时从磁盘加载。
```

- [ ] **Step 3: 提交全部改动**

```bash
git add third_party/serp/baidu_serp.js src/RenderTask.h src/RenderQueue.h src/RenderQueue.cpp src/RenderPool.h src/RenderPool.cpp src/HttpServer.cpp src/McpServer.cpp CMakeLists.txt AGENTS.md
git commit -m "feat: 百度搜索结果结构化提取（去广告），MCP web_search 应用"
```

---

## 完成标准

全部 Task 打勾后，以下行为成立：
1. `POST /render {url:baidu, output:html, extract:baidu_serp}` 返回 `serp_json`，含去广告的 results 数组。
2. MCP `web_search engine=baidu query="Python"` 返回结构化编号列表 + JSON 块，无广告。
3. MCP `web_search engine=bing` 行为与改造前一致（回归）。
4. WS `render` 不带 extract 时行为不变（回归）。
5. 百度反爬拦截页触发时返回 `blocked:true` 报错文案。
6. `baidu_serp.js` 缺失时静默降级（返回 raw html，服务不崩）。
