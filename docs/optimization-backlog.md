# SeimiRender 优化提升方向（真实使用驱动）

来源：2026-09-11 用 SeimiRender 代替人工分析 Jira（`jira.gwm.cn` 工单筛选：A级 / 高层评车标识）
的一次完整真实会话。以下条目均为该会话中实际撞到的问题，按价值排序。修复/实现时从这里认领，
完成后把条目移到文末「已完成」并注明 commit。

2026-09-14 补充：第二次真实会话（渲染单个内网 Jira 工单 CUXV5VR-11656，全程走 MCP 通道），
新增条目 6-9。

> **2026-09-15 批量处置**：条目 1/2/3/5/6/7/9 已实现（v1.3.0，见文末「已完成」）；
> 条目 4 此前已修复；条目 8 经决策**不做**（维持人工机制），原需求描述移入「已决策不做」。

## 1. JSON / REST 端点直取能力（fetch 模式）—— ✅ 已完成

见文末「已完成」#1。

## 2. 终态暴露 final_url + 登录墙检测 —— ✅ 已完成

见文末「已完成」#2。

## 3. 代理 scheme 拼写错误不再静默回退 —— ✅ 已完成

见文末「已完成」#3。

## 4. 留档：cookie 持久化 PKCS7 padding bug（已修复）

- **现象**：重启后 cookie 恢复一直静默失败（0 条），长期被「重启后手动重新同步」掩盖，
  直到本次「设置了 --data-dir 为何不自动加载」的排查才暴露。
- **根因**：qaes 成员 `decode()` 返回带 PKCS7 padding 的明文，`CookieStore::decryptData`
  未去 padding 直接喂 `QJsonDocument::fromJson`，尾部 padding 字节触发
  "garbage at the end" 静默解析失败。
- **修复**（2026-09-11）：`removePadding` + 失败告警 + `saveToDisk` 原子写。
- **留档原因**：这是「真实使用暴露 + 全链路静默吞错」的典型案例——加密、MAC、文件全都是好的，
  断在最后一步解析且零日志。新增持久化/解析路径时以此为鉴：失败必须可见。

## 5. 使用经验：重 JS 页面（仪表盘类）的采集参数 —— ✅ 已完成（并入 README）

见文末「已完成」#5。

## 6. 代理分流粒度：`--proxy` 全局生效，无 bypass/no_proxy 列表（2026-09-14 实撞）—— ✅ 已完成

见文末「已完成」#6。

## 7. 网络类失败的错误信息缺自诊断上下文（Agent 不可自愈）—— ✅ 已完成

见文末「已完成」#7。

## 8. MCP 工具集缺登录态管理能力（新实例/新机器冷启动撞登录墙）—— ⛔ 已决策不做

- **原诉求**：MCP 增加 `set_cookies`（注入指定域 cookie）/ 可选 `export_session`（跨实例
  搬运会话），让 agent 冷启动自助建立登录态。
- **决策**（2026-09-15 方案评审）：不做。维持既有**人工机制**——浏览器插件一键同步 /
  HTTP `POST /cookies` / 文件级搬运 `cookies.dat + seimi.key`。agent 侧通过条目 2 落地的
  `auth_wall` 检测感知掉登录，然后**提示用户人工重同步**即形成闭环，无需 agent 直接写 cookie。
  `export_session`（导出全量 cookie 明文）安全面过大，同样不做。
- **若未来复现高频需求**：优先考虑把插件同步入口做成 agent 可触发的本地提示流，再评估
  MCP 注入通道。

## 9. MCP serverInfo.version = "unknown"（一行修复）—— ✅ 已完成

见文末「已完成」#9。

## 2026-09-14 会话验证记录（能力侧，无需改代码）

- `exec_js` + 页面同源 `fetch()` 调 Jira REST API：结构化工单数据直出（key/status/assignee/
  labels/issuelinks/description），带登录态 cookie，零 HTML 反解——条目 1（fetch 模式）的临时
  绕法已可用，但仍是整页管线（~6s），条目 1 的轻量取数提速诉求不变。
- Jira 工单页 `readability` 显著优于 `conservative`（跳过头像 base64/编辑器工具栏噪音，字段完整）。

## 2026-09-14 二次排障留档：企业 VPN（MotionPro 等）对代理链路的选择性绞杀

- **现象**：Clash 的 `nameserver-policy` 已正确指向内网 DNS（mihomo 解析正常），但走
  socks5 入站的内网请求仍 3ms 内被掐；direct 模式也因系统解析回 fake-ip 一并失败。唯一幸存
  路径是「不绑接口的自然路由直连」。
- **根因**：终端同时挂着 MotionPro（Array VPN）的网络扩展时，**显式绑定出接口/源地址的
  TLS 出站**（mihomo 的 Go socket bind、curl `--interface`）会被本机代答 TCP 后掐断——
  3ms 内完成「握手+RST」，小于真实 RTT（15ms），物理上只能是本机行为；纯 TCP（nc）与
  不绑定的连接不受影响。同时 VPN 会扰动 macOS scoped resolver，内网 DNS 抢答失效，
  系统/Chromium 解析内网域拿到 Clash fake-ip，直连也掉坑。
- **可操作结论**：企业 VPN 与 TUN 代理并存是高危组合。SeimiRender 在此类环境优先 direct
  模式 + 让用户给 Clash `fake-ip-filter` 加内网域（恢复真实 IP 应答）；`--proxy` 模式在
  「VPN 拦截显式绑定」环境下无解，错误表现即 `ERR_CONNECTION_CLOSED`（毫秒级瞬断特征
  可作为排障指纹：真服务器慢速 RST vs 本机安全软件 3ms 代答掐断）。
- **错误信息增强方向**（并入条目 7）：failed 原因里附 resolved IP——若为 198.18.0.0/15
  （fake-ip 段）即可直接提示「目标被 fake-ip 接管，查 dns.nameserver-policy/fake-ip-filter」。

---

## 已完成

（从上方移入：条目 + 修复 commit。以下 2026-09-15 批次随 v1.3.0 实现，commit 待用户确认后回填）

**#1 JSON/REST 直取（fetch 模式）**：`POST /fetch` 端点（HttpServer）+ MCP `fetch_url` 工具 +
`CookieStore::cookiesForHost()` 按域取 cookie API。网络栈按评审决策用 QNetworkAccessManager
（栈上事件循环同步等待，HarRecorder 同款先例），自动跟随全局代理（http/socks5）与
`--proxy-bypass` 分流，与页面渲染网络路径一致。带 `cookies_applied`/`final_url`/`body_truncated`
可观测字段，Metrics 计入 host 维度。README 新增「轻量直取」章节。

**#1 增补（2026-09-15 真实使用暴露，fetch 附件场景）**：
- **崩溃修复**：用 /fetch 连续下载 5-6MB 二进制附件（Jira tar.gz）稳定 SIGSEGV。根因是
  「httplib 线程栈上 QNAM + 栈上 QEventLoop」模式：adopted 线程的事件分发器惰性创建后
  永不销毁，栈上 QNAM 析构后其连接池/定时器残留事件在下一次同线程事件循环命中已释放
  对象；小 JSON 常侥幸，大响应体稳定复现。重构为 **FetchService**（src/FetchService.{h,cpp}）：
  专职 QThread + 长命 QNAM + 全异步回复状态机，httplib 线程仅经条件变量等待（等待上限
  = timeout + 15s 兜底）。回归验证：12 轮混合模式拉 5.6MB 二进制 + 6 路并发 base64
  字节级完整，进程存活。
- **二进制输出**（用户提的改进建议，直接实现）：`response_format` 参数——`text`（默认，
  JSON 内嵌，二进制有损）、`base64`（二进制安全，`body_base64`+`body_bytes`，**>50MB 返回
  413 拒绝**并提示用 file）、`file`（落盘 `data-dir/fetch/`，返回 `saved_path`+`bytes`；
  `filename` 参数净化为 basename 防路径穿越，冲突加序号）。MCP `fetch_url` 透传
  `response_format`。回测 L8/L9 覆盖（base64 解码一致性+ICO 魔数；file 落盘+文件名净化）。

**#2 final_url + auth_wall**：`RenderWorker` 在 collect 起点采集 `page->url()`（失败路径现场取），
`auth_wall` 用保守 URL 路径启发式（login/signin/sso/oauth/cas/auth* 段，刻意不做内容特征防误报）。
字段链路：RenderTask → RenderResult/Snapshot → `/result`/`/render` 响应 + WS `finished` 推送 +
MCP `render_url` 响应头（命中时附醒目提示，指导 agent 让用户重同步 cookie）。

**#3 代理 scheme 告警 + 自动纠正**：`parseProxyUrl` 对未知 scheme 先查常见拼写纠错表
（socket5/socks5h/sock5/socks4 → socks5，覆盖编辑距离 2 的真实 typo）+ 编辑距离 ≤1 兜底，
纠正/回退均打 WARN；`https://` 特例明确提示不支持按 HTTP 处理；解析失败退化 Direct 的路径补
WARN（原先完全静默）。

**#5 采集参数最佳实践留档**：经用户决策（2026-09-15）**不并入 README**（README 保持对外 API 文档的定位），经验留档于本文件：仪表盘类 `settle_ms>=6000`、表格优先 markdown、REST 用 /fetch、readability 优先于 conservative；TUN 别配 --proxy、VPN+TUN 高危、fake-ip 排障指纹、代理切换 1min 过渡期；auth_wall / `cookies_applied` 的登录态健康度判读。

**#6 --proxy-bypass 分流**：`--proxy-bypass <domain,cidr,...>` 参数，**双栈实现**（关键实测发现：
QtWebEngine 渲染网络栈不按请求查询 `QNetworkProxyFactory`（只轮询全局单值 applicationProxy），
backlog 原方向「QtWebEngine 尊重 proxy factory」被实证否定）：
- 渲染侧：main.cpp 在 bypass 模式下注入 Chromium 原生 `--proxy-server` +
  `--proxy-bypass-list`（经 `QTWEBENGINE_CHROMIUM_FLAGS`，实测对当前 Qt 构建的网络服务生效
  ——旧记录「该 env 到不了网络服务」同样被实证推翻）。代价：flag 是启动期快照，bypass 模式下
  渲染代理不随 POST /proxy 热切换（README/AGENTS 已注明；需热切换就别用 bypass）。
- /fetch 侧：`ProxyConfig::fetchProxyFactory()` 返回**实例级** SeimiProxyFactory，只挂 /fetch
  专属 QNAM（QNAM 按请求查 factory，分流与渲染行为一致）。**绝不全局注册**——全局 factory 会让
  applicationProxy() 变成 factory 对空 host 查询的返回值，QtWebEngine 轮询到就推送「无 bypass
  规则的 fixed 代理」覆盖 Chromium flag（回测 M 组实撞后修正）。
回环（localhost/127.0.0.1/::1）永远直连。未配置 bypass 时保持 setApplicationProxy 全局单值
路径零行为变更。README 记录 TUN 反模式经验。回测覆盖：regression_test.py M 组（M1 scheme
纠错 WARN / M2 初始代理回显 / M3 bypass 域名穿死代理直连成功 / M4 非 bypass 走死代理失败带
ERR_PROXY+诊断 / M5 fetch 分流一致；注意 example.com/org/net 是 Chromium 隐式永真直连域，
不能用作 bypass 验证目标）。

**#7 网络失败自诊断**：`UrlGuard::urlSsrfCheck` 出参透出解析 IP（--no-ssrf 下用新增
`resolveHostIp` 仍解析一次），随任务存 `resolvedIp`；网络类失败（chrome-error 页 / body too
small）的 error 追加 `[proxy=... | resolved=... | hint: ...]`：fake-ip 段（198.18.0.0/15）提示
查 fake-ip-filter；代理开 + 目标私网提示 bypass/direct；`ProxyConfig` 记录
`lastAppliedChangeMsec`，切换后 <60s 的失败提示过渡期。评审决策：**仅提示不自动重试**。

**#9 MCP serverInfo.version**：根因是 `SEIMI_VERSION` 宏只注入 `seimi-render` 可执行目标、
未注入 `seimi-mcp` 静态库（McpServer.cpp 编译进库时宏未定义走 "unknown" 兜底）。CMakeLists
给 seimi-mcp 的 `target_compile_definitions` 补注入，一行修复。

**#4 cookie padding bug**：2026-09-11 已修复（removePadding + parseStore 返 bool + 原子写 +
三条失败路径 qWarning），详见上方留档。
