# 百度搜索结果结构化提取 — 设计文档

- 日期：2026-07-01
- 状态：待 review
- 落点：`third_party/serp/baidu_serp.js`（新增）、`src/RenderTask.h`（新增枚举与字段）、`src/RenderQueue.{h,cpp}`（Result/Snapshot/submit 签名扩展）、`src/RenderPool.{h,cpp}`（第三条提取路径）、`src/HttpServer.cpp`（新增参数与响应字段）、`src/McpServer.cpp`（web_search 编排）、`CMakeLists.txt`（install 规则）、`AGENTS.md`（文档）

## 1. 背景与目标

`web_search` 工具（`src/McpServer.cpp:434-541`）已支持 `engine=baidu`，但只把整张百度结果页用 `conservative` markdown 一股脑返回：**广告、百度自家产品卡片、右侧推荐栏、"其他人还在搜"、页脚分页全部混在一起**，对下游 LLM 是强噪声。

本次目标：让 `web_search engine=baidu` 返回**结构化、去广告**的搜索结果列表——每条结果含标题、真实 URL、摘要、来源，剔除广告与推荐噪声；并为 google/bing/duckduckgo 预留同样的可扩展框架（本次只实现百度解析，其余维持现状）。

### 关键约束（已确认）

| 决策点 | 选择 |
|---|---|
| 返回格式 | 结构化 text content（含 JSON 摘要 + 可读编号列表） |
| 广告/卡片 | 剔除广告；保留自然结果 + 有用的百度聚合卡（标注来源） |
| 解析位置 | 新增 site-specific JS 提取脚本，在渲染层 DOM 内运行 |
| 范围 | 百度优先；为其它引擎预留 per-engine 可插拔框架 |
| 提取时机 | 渲染后取 HTML/DOM，在 RenderWorker 内注入 JS 解析（不另发请求） |
| URL 去跳转 | `mu` 属性优先；缺失或为 `baidu.com/link?url=` 时原样保留并标 `is_redirect` |

### 百度 SERP HTML 结构（已用 seimi-render 真实渲染验证，2026-07-01 实测）

> **数据来源**：用 seimi-render 自身（真实 Chromium + stealth.js）渲染 `https://www.baidu.com/s?wd=Python`，拿到 1.5MB 真实 SERP HTML + 841KB 截图（curl/PowerShell 直取只能拿到 1.5KB 反爬拦截页，证实渲染层的不可替代性）。以下选择器全部基于该真实 DOM 实测，交叉参考 ohblue/baidu-serp-api 生产解析器。百度 HTML 随版本迭代会变，**选择器以"多信号兜底"设计，不依赖单一 class**。

**关键实测发现（推翻部分第三方文档的过时说法）：**

| 结构 | 作用 | 处理 | 实测命中 |
|---|---|---|---|
| `div.result.c-container[tpl="www_index"]` | **最新自然网页结果**容器 | ✅ 保留 | 4 个（`www_struct`/`www_normal` 本次为 0，但保留兜底） |
| `div.result-op.c-container` | 特殊/聚合卡片（百科/翻译/贴吧/实体卡等） | ⚠️ 保留但标 `type:"baidu_card"` | 6 个 |
| `tpl="recommend_list"` | **"其他人还在搜"聚合** | ❌ 剔除 | 1 个 |
| 容器内 `h3 > a` | 标题 + 跳转链接 | ✅ 标题来源 | — |
| 容器 `mu` 属性 | **真实目标 URL**（百度埋点，比 link 跳转准） | ✅ URL 首选 | 12 个 |
| `span[class*="summary-text"]`（实测 `summary-text_560AW`，**class 末尾有换行空格**） | 摘要文本 | ✅ | 8 个 |
| `span.cosc-source-text`（精确 class，含 `cos-line-clamp-1`） | 来源站点名 | ✅ | 6 个 |
| **广告容器**：class 含 `EC_result` / `ec-pc-huanxin-grid` / `ec-pc-fresh-font-color` | **广告**（实测） | ❌ 剔除 | 5 个 |
| **广告角标**：子元素 `<span class="ec-tuiguang">` | 广告标识 span（最可靠信号） | ❌ 剔除依据 | 6 个 |
| 广告 data 属性：`data-srcid`/`data-placeid`/`data-cmatchid`/`data-bidword` | 广告专属埋点（区别于自然的 `srcid=`） | ❌ 剔除依据 | — |
| `id="content_left"` | 主结果区（限定解析范围） | ✅ 范围 | 1 个 |
| `id="rs"` | 相关搜索区 | 单独提 `recommend` | 1 个 |
| `<title>` 含"安全验证" 或 body.innerText < 50 字 | **反爬拦截页**（非真实 SERP） | ❌ 报错 `baidu_blocked` | — |

**⚠️ 推翻的过时假设（spec 初稿基于第三方文档，实测修正）：**

1. **广告判定不能用 `tuiguang` 在容器 class 里**：实测 `tuiguang` 出现在子 `<span class="ec-tuiguang">` 角标和 CSS 里（`<style>.ec-tuiguang{...}`），**不在广告容器外层 class**。可靠信号是 `EC_result`/`ec-pc-huanxin-grid` class + `ec-tuiguang` 子 span。
2. **没有字面"广告"/"推广"可见文本**：实测整页 0 处"广告"/"推广"字样——百度要么动态渲染角标、要么用图标。文本判定无效。
3. **`www_struct`/`www_normal` tpl 当前为 0**：只有 `www_index` 命中。保留旧 tpl 作兜底，但不依赖。
4. **广告容器内嵌套重复内容**：广告 div 内部有与外层同标题的嵌套 div（带 `c-container` 但无 `srcid`）。**靠标题去重 + `srcid` 存在性过滤**避免重复/误收。

## 2. 总体架构

### 2.1 数据流

```
MCP web_search(engine=baidu, query)
  │
  │  POST /render {url:baidu, output:"html,serp_json", extract:"baidu", ...}
  ▼
HttpServer /render handler
  │  解析 extract 参数 → ExtractAlgorithm::BaiduSerp
  │  RenderQueue::submit(task)  task 带 extractAlgorithm
  ▼
RenderWorker (GUI 线程)
  │  load(url) → loadFinished → settle
  │  onSettleElapsed 分支：
  │    if task.extractAlgorithm == BaiduSerp → extractSerp()  ← 新增
  │  extractSerp(): 注入 baidu_serp.js → 读 chunk → 存 m_serpJson → collect()
  │  collect(): 照常 toHtml
  │  tryFinishCollect(): result.serpJson = m_serpJson  ← 新增透传
  ▼
HTTP 响应 {state, html, markdown, serp_json:{results:[...], blocked, ...}}
  │
  ▼
McpServer web_search handler
  │  解析 serp_json：
  │    blocked=true  → 报错提示（建议重试/换引擎/加超时）
  │    results=[]    → "未找到结果"
  │    results 非空  → 组装结构化 text content 返回
  ▼
LLM 收到干净的编号结果列表
```

### 2.2 为什么放在渲染层而不是 MCP 层

用户选择"新增 site-specific JS 提取脚本"。深层原因：

1. **JS 跑在真实 Chromium DOM 上**：百度结果大量内容由 JS 动态渲染，`mu`/`tpl` 等属性在静态 HTML 字符串里可能缺失或乱序。在 `m_page` 的 DOM 上 `querySelectorAll` 拿到的是 Chromium 渲染后的真实结构，最可靠。
2. **遵循项目既有架构**：项目已有 `extract.js`(Readability) + `simplify.js`(conservative) 两条"JS 从磁盘加载 → 注入 → chunk 回读"的成熟路径（`RenderPool.cpp:368-504`）。`baidu_serp.js` 是第三条同构路径，复用同一套 chunk 协议与缓存机制，零新基础设施。
3. **百度改版只改 JS 不重编 C++**：百度 HTML 结构会变（`summary-text_560AW` 这种带 hash 的 class 几个月一换）。JS 脚本随二进制 install 到 `third_party/serp/`，改版时改 JS 文件即可，不必重新编译 C++。
4. **MCP 层不做脆弱的字符串正则**：MCP 拿到的是渲染后的结构化 JSON，编排逻辑（组 text content、去广告已在前序完成）保持简单。

### 2.3 per-engine 可插拔框架（为 google/bing 预留）

- `ExtractAlgorithm` 枚举（见 3.1）：`None`(默认) / `BaiduSerp` / 预留 `GoogleSerp`/`BingSerp`。
- 渲染层只认枚举值 → 查表找对应 JS 文件名（`baidu_serp.js`/`google_serp.js`/...），文件不存在则降级为不提取（返回原始 html/markdown，不阻断）。
- 新增引擎 = 新增一个枚举值 + 一个 JS 文件 + McpServer `web_search` 里一处 URL 模板与 engine 匹配，**不改 RenderWorker 状态机**。
- 本次只实现 `BaiduSerp`；`GoogleSerp`/`BingSerp` 枚举值可暂不定义（YAGNI），等真要加时再补，框架已就位。

## 3. 关键技术实现细节

### 3.1 RenderTask.h — 新增枚举；RenderQueue — Result/Snapshot 字段串联

**RenderTask.h** 新增枚举与请求侧字段：

```cpp
// 提取算法（独立于 MdAlgorithm，作用于"识别页面类型并结构化抽取"）
enum class ExtractAlgorithm : std::uint8_t {
    None,        // 默认：不做 SERP 提取（现有行为不变）
    BaiduSerp,   // 百度搜索结果页结构化提取
    // 预留：GoogleSerp, BingSerp, DuckDuckGoSerp —— 框架就位后按需添加
};
```

`RenderTask` 新增不可变字段 `const ExtractAlgorithm extractAlgorithm = ExtractAlgorithm::None;`（构造函数默认 `None`，现有所有调用点零改动）。

**关键：结果数据要经 `RenderQueue::RenderResult` → `Snapshot` 才能到达 HTTP 响应。** `HttpServer::jsonStateResp` 只读 `Snapshot`（`HttpServer.cpp:222`），看不到 `RenderTask` 也看不到 `RenderResult`。因此 `serpJson` 必须贯穿三个结构体（spec 自审发现的原稿遗漏点）：

| 结构体 | 位置 | 新增字段 | 作用 |
|---|---|---|---|
| `RenderQueue::RenderResult` | `RenderQueue.h:52-60` | `QString serpJson;` | `tryFinishCollect` 写入（RenderPool 产出） |
| `RenderTask`（mutable） | `RenderTask.h` mutable 区 | `QString serpJson;` | `reportSucceeded` 从 Result 拷入任务终态 |
| `RenderQueue::Snapshot` | `RenderQueue.h:65-83` | `QString serpJson;` | `snapshot()` 加锁拷出，供 `jsonStateResp` 读取 |

数据流（必须完整接通，否则 `serp_json` 永远到不了响应）：
```
RenderWorker.tryFinishCollect()  → result.serpJson = m_serpJsonResult   (result 是 RenderResult，不是 RenderTask)
RenderQueue::reportSucceeded()   → task->serpJson = result.serpJson     (写入任务终态)
RenderQueue::snapshot()          → snap.serpJson = task->serpJson       (加锁拷出)
HttpServer::jsonStateResp()      → 读 s.serpJson，非空则注入响应
```

### 3.2 baidu_serp.js — 提取脚本（核心）

落点：`third_party/serp/baidu_serp.js`。结构与 `simplify.js`/`extract.js` 同构：IIFE，结果存 `window.__seimiSerp`，chunked 协议回读，返回小状态 JSON。

伪代码（实际 JS 实现见实现计划）：

```js
(function () {
  function extractBaiduSerp() {
    var out = { engine: "baidu", blocked: false, results: [], recommend: [], meta: {} };

    // 1. 反爬拦截页检测：title 含"安全验证" 或 body 极短（实测拦截页仅 ~1.5KB）
    var t = document.title || "";
    var bodyText = (document.body && document.body.innerText) || "";
    if (/安全验证/.test(t) || bodyText.length < 50) {
      out.blocked = true;
      return stash(out);
    }

    // 2. 限定主结果区（实测：所有结果都在 #content_left 内）
    var scope = document.querySelector('#content_left') || document.body;

    // 3. 收集候选容器：所有 c-container（自然 + 卡片 + 广告都在内）
    var containers = scope.querySelectorAll('div.c-container');

    var seenTitles = {};  // 嵌套去重（广告 div 内嵌套重复标题）
    var adCount = 0;

    containers.forEach(function (c) {
      var cls = c.className || "";
      var mu = c.getAttribute("mu") || "";
      var tpl = c.getAttribute("tpl") || "";

      // 3a. 广告判定（实测可靠信号）
      //     - 容器 class 含 EC_result / ec-pc-huanxin-grid / ec-pc-fresh-font-color
      //     - 子元素有 <span class="ec-tuiguang"> 角标
      //     （实测：tuiguang 不在容器 class，在子 span；无"广告"字面文本）
      var isAd = /EC_result|ec-pc-huanxin|ec-pc-fresh-font-color/.test(cls) ||
                 !!c.querySelector('span[class*="ec-tuiguang"]');
      if (isAd) { adCount++; return; }

      // 3b. recommend 聚合剔除
      if (tpl === "recommend_list") return;

      // 3c. 标题
      var h3a = c.querySelector('h3 a') || c.querySelector('h3');
      if (!h3a) return;
      var title = (h3a.innerText || h3a.textContent || "").trim();
      if (!title || seenTitles[title]) return;  // 嵌套去重
      seenTitles[title] = true;

      // 3d. URL：mu 优先（实测自然结果 mu=真实URL，如 python.org）
      //     mu 缺失或为 baidu.com/link/nourl.ubs → 跳转链，标 is_redirect
      var href = (h3a.getAttribute && h3a.getAttribute("href")) || "";
      var url = mu || href;
      var isRedirect = !mu || /baidu\.com\/link\?url=/.test(url) || /nourl\.ubs\.baidu\.com/.test(url);

      // 3e. 摘要（实测 summary-text_560AW，class 末尾常有换行空格，用前缀通配）
      var desc = "";
      var descEl = c.querySelector('span[class*="summary-text"], .c-abstract, span[class*="content-right"]');
      if (descEl) desc = (descEl.innerText || "").trim();

      // 3f. 来源（实测 cosc-source-text 精确命中）
      var source = "";
      var srcEl = c.querySelector('.cosc-source-text, .c-showurl');
      if (srcEl) source = (srcEl.innerText || "").trim();

      // 3g. 类型：result-op 为百度聚合卡
      var isCard = /result-op/.test(cls);

      out.results.push({
        title: title, url: url, is_redirect: isRedirect,
        snippet: desc, source: source,
        type: isCard ? "baidu_card" : "organic"
      });
    });

    // 4. 相关搜索（#rs 区，单独入 recommend，不混入 results）
    var rs = document.querySelector('#rs');
    if (rs) rs.querySelectorAll('a').forEach(function (a) {
      var tx = (a.innerText || "").trim();
      if (tx) out.recommend.push(tx);
    });

    out.meta = { count: out.results.length, ads_filtered: adCount };
    return stash(out);
  }

  function stash(obj) {
    var s = JSON.stringify(obj);
    var CHUNK = 30000, chunks = [];
    for (var i = 0; i < s.length; i += CHUNK) chunks.push(s.substring(i, i + CHUNK));
    window.__seimiSerp = { chunks: chunks };
    return JSON.stringify({ ok: true, totalChunks: chunks.length });
  }

  try { return extractBaiduSerp(); }
  catch (e) { window.__seimiSerp = { chunks: ["" + e] }; return JSON.stringify({ ok: false, error: "" + e }); }
})();
```

**设计要点（实测校准）：**
- **广告判定用 `EC_result`/`ec-pc-huanxin-grid`/`ec-pc-fresh-font-color` class + `ec-tuiguang` 子 span**——实测可靠，**不**依赖容器 class 里的 `tuiguang`（那不在容器上）也**不**靠"广告"字面文本（实测为 0）。
- **嵌套去重**：广告 div 内部嵌套同标题 div，用 `seenTitles` 去重避免重复/误收。
- **摘要用 `[class*="summary-text"]` 通配**：实测 class 是 `summary-text_560AW`（带 hash 后缀 + 末尾换行空格），通配匹配最稳。
- **来源用 `.cosc-source-text`**：精确命中（实测 6 个来源全命中），`.c-showurl` 作兜底。
- **拦截页检测**：title 含"安全验证"或 body 文本 < 50 字（拦截页极短）。

### 3.3 RenderPool.cpp — 第三条提取路径

新增成员与函数，与 `extractReadability`/`extractSimplifiedHtml` 完全同构：

```cpp
// RenderPool.h 新增（private）
QString m_serpJs;                    // 从磁盘加载的 baidu_serp.js（单实例缓存或 static 均可）
QString m_serpJsonResult;            // 提取出的 JSON
static QString s_serpJsCache;        // （可选）静态缓存，与 s_readabilityJs 同模式
void extractSerp();                  // 注入 baidu_serp.js + chunk 回读
void readSerpChunks(int idx, int total, const QString& acc);
```

settle 回调（懒加载图片激活后的 `QTimer::singleShot(2500,...)` lambda，`RenderPool.cpp:349-360`）的分支扩展为三分支。注意：**代码里没有名为 `onSettleElapsed` 的方法**——它是嵌在懒加载激活回调里的内联 lambda，原稿沿用了不准确的命名，这里更正：

```cpp
QTimer::singleShot(2500, this, [this]() {
    if (!m_busy || !m_task || !m_page) return;
    if (m_task->extractAlgorithm == ExtractAlgorithm::BaiduSerp) {
        extractSerp();              // 新增：SERP 结构化提取
    } else if (m_task->wants(Output::Markdown)) {
        // 现有 Readability / conservative 分支不动
    } else {
        collect();
    }
});
```

**注意**：SERP 提取与 Markdown 提取**互斥**（一个 SERP 页面要么结构化提取，要么转 markdown，不会同时）。当 `extract=BaiduSerp` 时，web_search 只请求 `output:"html"`，不请求 markdown。

> **自审更正**：原稿把互斥的理由写成"避免对 `m_pendingCollect` 计数器的并发干扰"，这是**不准确的**。经核查 `collect()`(`RenderPool.cpp:510-544`) / `decCollect()`(578-581) / `tryFinishCollect()`(584-587)：`m_pendingCollect` 只计 html/pdf/screenshot 三项，markdown 转换是在计数归零后于 `tryFinishCollect` 内同步进行、**不计入计数**；`extractSerp()` 跑在 `collect()` 之前、走 `runJavaScript` 异步回调、**根本不碰计数器**。所以两者同跑不会破坏计数、不会死锁。真正的互斥理由是**产品决策**：SERP 页转出的 markdown 是纯噪声（本特性要消除的就是它），且省掉一次无意义的 `html2md::Convert`。互斥是对的，但与计数器安全无关。

**`tryFinishCollect()`（`RenderPool.cpp:584-640`）末尾新增透传**——注意这里的 `result` 是 `RenderQueue::RenderResult`（局部变量，`RenderPool.cpp:589`），**不是 `RenderTask`**（原稿此处描述有误，已更正）：

```cpp
if (m_task->extractAlgorithm != ExtractAlgorithm::None && !m_serpJsonResult.isEmpty()) {
    result.serpJson = m_serpJsonResult;   // result 是 RenderResult，见 3.1 数据流
}
```

`extractSerp()` 实现（与 `extractSimplifiedHtml` 几乎逐行对称）：
- 从 `<appdir>/third_party/serp/baidu_serp.js` 或 `SEIMI_ADMIN_UI_SRC_DIR/../third_party/serp/baidu_serp.js` 加载（首次加载缓存）。
- `m_page->runJavaScript(s_serpJsCache, callback)`，解析返回的 `{ok,totalChunks}`。
- `readSerpChunks` 逐 chunk 读 `window.__seimiSerp.chunks[idx]`，拼成 `m_serpJsonResult`。
- 完成后调 `collect()`（照常取 html，作为 fallback 与诊断用）。

`tryFinishCollect()`（`RenderPool.cpp:598-615`）末尾新增透传：
```cpp
if (m_task->extractAlgorithm != ExtractAlgorithm::None && !m_serpJsonResult.isEmpty()) {
    result.serpJson = m_serpJsonResult;
}
```
（`result` 是 `RenderTask`，serpJson 字段在 3.1 已加。）

### 3.4 HttpServer.cpp — 新增参数与响应字段

`/render` 新增可选参数 `extract`（`src/HttpServer.cpp:815-822` 附近）：

```cpp
// 站点特定提取算法：baidu_serp（百度搜索结果页结构化）。默认 none。
std::string extractStr = obj.value("extract").toString().toStdString();
ExtractAlgorithm extractAlg = parseExtractAlgorithm(extractStr);  // 新增解析函数
```

`parseExtractAlgorithm`（与 `parseMdAlgorithm` 同模式）：`"baidu_serp"`/`"baidu"` → `BaiduSerp`，其余 → `None`。

`RenderQueue::submit` 签名扩展（新增 `extractAlg` 参数，默认 `None`）。当前签名（`RenderQueue.h:43-44`）：
```cpp
QString submit(QString url, int settleDelayMsec, OutputMask outputs,
               ImageFormat imageFormat, MdAlgorithm mdAlgorithm, qint64 nowMsec);
```
新增尾部 `ExtractAlgorithm extractAlgorithm = ExtractAlgorithm::None`，两个调用点（`HttpServer.cpp:824`、`WsServer.cpp:203`）靠默认值零改动。

响应 JSON（`jsonStateResp`，`HttpServer.cpp:222-266`）新增字段。

> **嵌入机制（自审更正）**：`jsonStateResp` 用**裸字符串拼接 + `esc()` 转义**构建响应（非 JSON 库），而 `s.serpJson` 本身**已经是合法 JSON 字符串**。因此注入时**不能再过 `esc()`**（否则会把内部的 `"` 转成 `\"` 破坏结构），要裸嵌入：
> ```cpp
> if (!s.serpJson.isEmpty()) {
>     out += ",\"serp_json\":" + s.serpJson.toStdString();  // 裸拼，不 esc
> }
> ```
> 注意末尾**没有引号包裹**——`s.serpJson` 自身形如 `{"engine":"baidu",...}`，直接作为 JSON 值接在 `"serp_json":` 之后。

```json
{
  "state": "succeeded",
  "html": "...",
  "serp_json": {                      // 仅当 extract != none 且提取成功时存在（裸嵌入的 JSON 对象）
    "engine": "baidu",
    "blocked": false,
    "results": [
      {"title":"...","url":"...","is_redirect":true,"snippet":"...","source":"...","type":"organic"}
    ],
    "recommend": ["相关词1","相关词2"],
    "meta": {"count": 10}
  }
}
```
`serp_json` 是裸嵌入的 JSON 对象（不是字符串），方便 MCP 端 `json::parse(resp)` 后直接 `j["serp_json"]` 取用。

### 3.5 McpServer.cpp — web_search 编排改造

`web_search` handler（`src/McpServer.cpp:469-541`）改造：

```cpp
// 1. baidu 引擎走结构化提取路径
bool isBaidu = el.find("baidu") != std::string::npos;
mcp::json body = { {"url", url}, {"settle_ms", settle}, {"long_poll_ms", timeout} };
if (isBaidu) {
    body["output"] = "html";        // 给 chunk 协议 + 兜底
    body["extract"] = "baidu_serp"; // 触发结构化提取
} else {
    body["output"] = "markdown";    // google/bing/ddg 维持现状
    body["md_algorithm"] = "conservative";
}
std::string resp = impl->httpPost("/render", body.dump());
// ... 解析 resp ...
auto j = mcp::json::parse(resp);

// 2. baidu 分支：消费 serp_json
if (isBaidu) {
    if (!j.contains("serp_json")) {
        // 提取未生效（JS 缺失/异常）→ 降级返回 html 的前若干字符作诊断
        return toTextContent("[web_search engine=baidu query=\"" + query + "\"]\n"
                             "(extraction unavailable; raw html returned)\n\n" +
                             j.value("html", "").substr(0, 2000));
    }
    auto sj = j["serp_json"];
    if (sj.value("blocked", false)) {
        return toTextContent("[web_search engine=baidu query=\"" + query + "\"]\n"
                             "BLOCKED: Baidu returned a security-verification page. "
                             "Suggestions: 1) retry after a few seconds; "
                             "2) try engine='bing'; 3) increase timeout_ms.");
    }
    auto results = sj.value("results", mcp::json::array());
    if (results.empty()) {
        return toTextContent("[web_search engine=baidu query=\"" + query + "\"]\n"
                             "(no results found)");
    }
    // 3. 组装结构化 text content：JSON 摘要块 + 可读编号列表
    std::string text = "[web_search engine=baidu query=\"" + query + "\" count=" +
                       std::to_string(results.size()) + "]\n\n";
    text += "```json\n" + results.dump(2) + "\n```\n\n";  // 结构化块，LLM 可精确解析
    text += "Results:\n";
    int idx = 1;
    for (auto& r : results) {
        text += std::to_string(idx++) + ". **" + r.value("title","") + "**";
        if (!r.value("source","").empty()) text += " — " + r.value("source","");
        text += "\n   " + r.value("url","");
        if (r.value("is_redirect", false)) text += "  (baidu redirect link)";
        if (!r.value("snippet","").empty()) text += "\n   " + r.value("snippet","");
        text += "\n";
        if (r.value("type","") == "baidu_card") text += "   _[baidu aggregated card]_\n";
    }
    auto rec = sj.value("recommend", mcp::json::array());
    if (!rec.empty()) {
        text += "\nRelated searches: ";
        for (size_t i=0;i<rec.size();++i) text += (i?", ":"") + rec[i].get<std::string>();
        text += "\n";
    }
    return toTextContent(text);
}
// 4. 非 baidu：维持现有 markdown 返回逻辑不动
```

### 3.6 安装与加载路径

`baidu_serp.js` 必须随二进制 install（与 `extract.js`/`simplify.js`/`stealth.js` 同待遇）：
- CMake `install()` 规则把 `third_party/serp/` 装到 `<bindir>/third_party/serp/`（仿 `extract.js`/`simplify.js` 的规则，`CMakeLists.txt:350-359`）。
- 开发期 `SEIMI_ADMIN_UI_SRC_DIR` 回退路径（与 `extractSimplifiedHtml` 的候选路径同构，`RenderPool.cpp:447-452`）同样加 `../third_party/serp/baidu_serp.js`。
- `AGENTS.md` 模块索引 / 资源坑位章节补一行说明 `third_party/serp/`。

## 4. 健壮性与降级

百度反爬与 HTML 多变是本特性的两大风险，设计上全部有降级路径：

| 风险 | 降级 |
|---|---|
| 百度返回安全验证拦截页 | JS 检测 → `blocked:true` → MCP 返回明确报错 + 建议（重试/换引擎） |
| `baidu_serp.js` 文件缺失/加载失败 | `extractSerp()` 仿照 `extractSimplifiedHtml`：文件缺失直接 `collect()`，`serpJson` 为空 → MCP 走降级分支返回 raw html 前 2000 字符诊断，不阻断 |
| JS 运行抛异常 | `try/catch` 兜底，存错误信息到 chunk，返回 `{ok:false}` → 同上降级 |
| 提取出 0 条结果（选择器全miss/百度改版） | MCP 返回 "(no results found)"，不崩溃 |
| 某条结果字段缺失（如无 snippet） | 每个字段独立 `|| ""` 兜底，不影响其它条目 |
| `mu` 属性缺失 | 回退 `h3>a href`，标 `is_redirect:true` |
| 单个 class 名变化（hash 后缀变了） | 多选择器兜底（`span[class*="summary-text"]` 通配匹配） |

**关键不变量**：SERP 提取**永不阻塞渲染主流程**。任何异常都收敛为"serpJson 为空 + 正常返回 html"，下游 MCP 有对应降级文案。符合铁律 2（GUI 线程回调绝不阻塞）—— JS 是 `runJavaScript` 异步回调，不阻塞。

## 5. 不在本次范围内（YAGNI）

- **不为 google/bing/duckduckgo 实现 SERP 解析**：本次只百度。框架（枚举 + 文件查表 + URL 模板）已就位，后续按需各加一个 JS 文件即可。
- **不自动跟随 `baidu.com/link?url=` 拿真实 URL**：每条结果多一次网络请求，慢且易被限。`mu` 已能覆盖绝大多数；缺失时标 `is_redirect` 让下游决策。
- **不做分页/翻页**（`pn` 参数）：单次返回首页结果已足够 LLM 决策；翻页由调用方用 `render_url` 自驱。
- **不解析百度资讯/图片/视频等垂直频道**：只解析通用网页搜索 `www.baidu.com/s?wd=`。
- **不改 `md_algorithm` 语义**：`extract` 是与 `md_algorithm` 正交的新维度（一个管"识别页面类型结构化抽取"，一个管"HTML→markdown 转换策略"），互不影响。
- **不为 SERP 提取加截图/PDF**：搜索结果无需截图，`output:"html"` 足够。

## 6. 文件改动清单

| 文件 | 改动类型 | 说明 |
|---|---|---|
| `third_party/serp/baidu_serp.js` | 新增 | 百度 SERP 提取脚本（约 120 行） |
| `src/RenderTask.h` | 新增枚举+字段 | `ExtractAlgorithm` 枚举、`RenderTask::extractAlgorithm`（不可变）、`RenderTask::serpJson`（mutable） |
| `src/RenderQueue.h` | 结构体扩展 | `RenderResult::serpJson`、`Snapshot::serpJson`；`submit` 签名加 `extractAlg` |
| `src/RenderQueue.cpp` | 数据流串联 | `submit` 传 `extractAlg` 进 task 构造；`reportSucceeded` 把 Result.serpJson 拷入 task；`snapshot()` 拷入 Snapshot |
| `src/RenderPool.h` | 新增成员+方法声明 | `m_serpJsonResult`、`extractSerp()`、`readSerpChunks()`、`s_serpJsCache` |
| `src/RenderPool.cpp` | 新增方法+分支 | 第三条提取路径；settle lambda（`RenderPool.cpp:349-360`）加分支；`tryFinishCollect` 末尾写 `result.serpJson` |
| `src/HttpServer.cpp` | 新增参数+解析+响应字段 | `extract` 参数、`parseExtractAlgorithm`、`jsonStateResp` 裸嵌入 `serp_json` |
| `src/McpServer.cpp` | web_search 改造 | baidu 走 extract 路径，消费 `serp_json` 组装输出 |
| `CMakeLists.txt` | install 规则 | `third_party/serp/` 装到 bindir（仿 `extract.js`/`stealth.js` 规则，`CMakeLists.txt:350-359`） |
| `AGENTS.md` | 文档 | 模块索引补 `third_party/serp/`，说明 SERP 提取机制 |

## 7. 验证方式

**已完成的预验证（2026-07-01）**：用 seimi-render 真实渲染 `baidu.com/s?wd=Python` 拿到 1.5MB 真实 SERP，用 Python 模拟 `baidu_serp.js` 的解析逻辑跑通，确认：5 个广告容器（`EC_result`/`ec-pc-huanxin-grid`）全部正确剔除、8 个有效结果（含 python.org/百度百科/百度翻译/菜鸟教程/公众号教程）正确提取、`mu` 属性取到真实 URL（python.org/baike.baidu.com 等）、`#rs` 相关搜索 10 词正确入 recommend。预验证脚本随实现删除。

实现后用 `scripts/regression_test.py` 的 MCP 组（G 系列）扩展，或直接手测：

```bash
# 启动服务（Linux/WSL 加 --no-sandbox；Windows 需 Qt bin 在 PATH）
./build/seimi-render --http-port 8088 --mcp-port 8090

# 直接测 HTTP 提取
curl -s localhost:8088/render -d '{"url":"https://www.baidu.com/s?wd=Python","output":"html","extract":"baidu_serp","long_poll_ms":45000}' | python -m json.tool
# 期望：serp_json.results 非空、blocked=false、无广告条目、ads_filtered>=1

# 测 MCP web_search（用回归脚本的 mcp_call 或任何 MCP 客户端）
# 调 web_search(engine=baidu, query="Python") 期望返回编号列表 + JSON 块
```

验证点：
1. 正常查询：`results` 非空，每条有 title/url，**无 `EC_result`/`ec-pc-huanxin-grid` 广告条目**（靠 class 判定，非"广告"字样）。
2. `mu` 存在时 `url` 为真实目标（如 `https://www.python.org/`），`is_redirect=false`；缺失/`nourl.ubs` 时 `is_redirect=true`。
3. `meta.ads_filtered >= 1`：证明广告被识别并剔除（Python 这类商业词必有广告）。
4. 反爬触发时（短时间高频请求复现）：返回 `blocked:true` 报错文案，不返回垃圾结果。
5. `baidu_serp.js` 故意改名后：降级返回 raw html 诊断文案，服务不崩。
6. google/bing 引擎 `web_search`：行为与改造前完全一致（回归无影响）。
