// seimi-render 控制台国际化（i18n）中枢
// 默认中文，右上角按钮切换中/英。语言选择持久化到 localStorage('seimi_lang')。
// 用法：
//   - 静态 HTML 元素加 data-i18n="key"（textContent）/ data-i18n-html="key"（innerHTML，
//     用于含 <code> 等标签的文案）/ data-i18n-placeholder="key"（input placeholder）
//   - JS 动态文本用 t('key')
//   - 切换语言调 setLang('en'|'zh')，会自动重渲染（app.js 监听后重跑各 load 函数）

(function () {
  'use strict';

  const STORAGE_KEY = 'seimi_lang';

  // ====== 字典 ======
  // 分组：common / login / tab / brand / stats / workload / proxy / render / cookies / mcp / docs / lang
  const DICT = {
    zh: {
      'common.loading': '加载中…',
      'common.noData': '暂无数据',
      'common.noSuccessTasks': '暂无成功任务',
      'common.copy': '复制',
      'common.copied': '已复制',
      'common.copyFail': '复制失败',
      'common.default': '默认',

      // 品牌与顶栏
      'brand.title': 'seimi-render 控制台',
      'brand.sub': '控制台',
      'conn.title': '连接状态',

      // 登录
      'login.hint': '控制台安全访问验证',
      'login.placeholder': '请输入管理密码',
      'login.submit': '登录系统',
      'login.wrongPwd': '密码错误',
      'login.connFail': '连接失败：',

      // 页签
      'tab.stats': '运行时统计',
      'tab.render': '渲染测试台',
      'tab.cookies': 'Cookie 状态',
      'tab.mcp': '配置示例',
      'tab.docs': '接口文档',

      // 统计页
      'stats.title': '渲染负载',
      'stats.realtime': '(实时)',
      'stats.cards.uptime': '运行时长',
      'stats.cards.total': '总请求',
      'stats.cards.success': '成功',
      'stats.cards.fail': '失败',
      'stats.cards.rate': '成功率',
      'stats.cards.throughput': '吞吐 req/s',
      'stats.latency.title': '延迟分布',
      'stats.latency.subtitle': '(成功任务 / ms)',
      'stats.outputs.title': '输出类型需求',
      'stats.outputs.screenshot': '截图',
      'stats.domains.title': '域名分布 Top',
      'stats.domains.topN': 'Top {n}（共 {total} 个域名）',
      'stats.domains.topNLimited': 'Top {n}',
      'stats.domains.colRank': '#',
      'stats.domains.colDomain': '目标域名',
      'stats.domains.colTotal': '总请求',
      'stats.domains.colSuccess': '成功',
      'stats.domains.colFail': '失败',
      'stats.domains.colRate': '成功率',
      'stats.domains.sortHint': '点击表头切换排序',
      'stats.domains.empty': '暂无数据',
      'stats.env.title': '运行环境',
      'stats.env.copy': '📋 拷贝',
      'stats.env.copied': '已拷贝',
      'stats.env.copyFail': '拷贝失败',
      'stats.env.os': '操作系统',
      'stats.env.kernel': '内核版本',
      'stats.env.hostname': '主机名',
      'stats.env.cpu': 'CPU',
      'stats.env.coresPhys': '物理核',
      'stats.env.coresLogical': '逻辑核',
      'stats.env.memory': '内存总量',
      'stats.env.gpuYes': '有',
      'stats.env.gpuNo': '无（headless 服务器）',
      'stats.env.qt': 'Qt 版本',
      'stats.env.build': '构建信息',
      'stats.env.cpuUsage': 'CPU 占用',
      'stats.env.rss': '内存占用',
      'stats.env.sampledAt': '采样于 {ts}',

      // 渲染负载
      'workload.title': '渲染负载',
      'workload.workerActive': 'Worker 活跃',
      'workload.queuePending': '队列堆积',
      'workload.queuePeak': '队列峰值',
      'workload.active': '活跃',
      'workload.poolNotStarted': '渲染池未启动',
      'workload.desc': 'Worker 总数为启动并发槽位（<code>--concurrency</code>），活跃数即正在渲染的任务数。队列堆积表示提交速度超过渲染速度；峰值（<code>peak_pending</code>）为历史最大堆积长度，单调不降，用于评估历史负载水位。',

      // 网络代理
      'proxy.title': '网络代理',
      'proxy.off': '未启用',
      'proxy.on': '直连',
      'proxy.socks5On': 'SOCKS5 代理 已生效',
      'proxy.httpOn': 'HTTP 代理 已生效',
      'proxy.genericOn': '代理 已生效',
      'proxy.kv.mode': '模式',
      'proxy.kv.directMode': '直连（不经过代理）',
      'proxy.directDesc': '所有渲染请求直接访问目标站点，未设置上游代理。如需经代理出网，可启动时加 <code>--proxy</code>，或运行时 <code>POST /proxy</code> 动态配置（即时生效，无需重启）。',
      'proxy.kv.protocol': '协议',
      'proxy.kv.host': '地址',
      'proxy.kv.port': '端口',
      'proxy.kv.user': '用户名',
      'proxy.kv.auth': '认证',
      'proxy.authSet': '已配置账号认证',
      'proxy.authNone': '无认证',
      'proxy.desc': '代理经由 <code>QNetworkProxy::setApplicationProxy</code> 运行时下发给 Chromium 网络栈（Qt WebEngine 轮询它，数秒内切换生效）。切换瞬间已在途的请求可能走旧代理，新请求走新代理。密码不回显。',

      // 渲染测试台
      'render.url': '目标 URL',
      'render.settle': '等待延迟 (Settle MS)',
      'render.longpoll': '长轮询超时 (Long Poll MS)',
      'render.outputs': '输出格式选择',
      'render.outHtml': 'HTML 结构',
      'render.outMd': 'Markdown 文本',
      'render.outPdf': '页面 PDF',
      'render.outShot': '截图',
      'render.shotFmt': '截图编码格式',
      'render.shotFmtHint': '（仅勾选截图时生效）',
      'render.shotFmt.auto': '自动',
      'render.shotFmt.autoHint': '（按图片占比智能选择）',
      'render.shotFmt.png': 'PNG',
      'render.shotFmt.pngHint': '（无损）',
      'render.shotFmt.jpg': 'JPEG',
      'render.shotFmt.jpgHint': '（照片页体积小）',
      'render.mdAlg': 'Markdown 正文算法',
      'render.mdAlgHint': '（仅勾选 Markdown 时生效）',
      'render.mdAlg.conservative': '保守',
      'render.mdAlg.conservativeHint': '（默认，零误伤）',
      'render.mdAlg.readability': 'Readability',
      'render.mdAlg.readabilityHint': '（正文定位，剔除非正文）',
      'render.extract': '站点特定提取',
      'render.extractHint': '（结构化提取搜索结果，与 Markdown 互斥）',
      'render.extract.off': '关闭',
      'render.extract.offHint': '（默认）',
      'render.extract.baidu': '百度搜索结果',
      'render.extract.baiduHint': '（去广告，结构化）',
      'render.extract.bing': '必应搜索结果',
      'render.extract.bingHint': '（结构化）',
      'render.extract.google': 'Google 搜索结果',
      'render.extract.googleHint': '（结构化）',
      'render.submit': '提交渲染任务',
      'render.submitting': '渲染中…',
      'render.btn': '渲染',
      'render.copyCurl': '拷贝 curl',
      'render.curlDone': '已拷贝 curl',
      'render.curlFail': '拷贝 curl 失败',
      'render.status.working': '渲染中…',
      'render.status.ok': '✓ 渲染成功（{ms} ms，task_id: {id}）',
      'render.status.fail': '✗ {state}：{err}（{ms} ms）',
      'render.status.failed': '渲染失败',
      'render.status.incomplete': '未完成',
      'render.status.reqFail': '请求失败：',
      'render.placeholder': '等待执行渲染...',
      'render.noOutput': '无输出',
      'render.pdfDownload': '⬇ 下载 PDF（{bytes} 字节）',
      'render.shotPreview': '截图预览（{fmt}，{bytes} 字节{note}）',
      'render.shotNote.jpeg': '（照片页体积优化）',
      'render.shotNote.lossless': '（无损）',
      'render.shotDownload': '⬇ 下载 {fmt}（{bytes} 字节）',
      'render.serp.blocked': '⚠ 反爬拦截',
      'render.serp.blockedDesc': '搜索引擎返回了安全验证页（非真实结果）。建议稍后重试或换引擎。',
      'render.serp.results': '搜索结果（{n} 条{ads}）',
      'render.serp.adsFiltered': '，已剔除 {n} 条广告',
      'render.serp.redirect': '(redirect)',
      'render.serp.rawJson': '查看原始 JSON',
      'render.htmlPreview': 'HTML 预览',
      'render.mdLabel': 'Markdown',

      // Cookie 页
      'cookies.title': '已同步 Cookie 概览',
      'cookies.desc': '服务端持有的渲染用登录态凭证（仅展示域名与数量，不涉及敏感明文）。',
      'cookies.clear': '清空当前会话',
      'cookies.purge': '永久删除',
      'cookies.encrypted': '🔒 已加密持久化到 <code>data/cookies.dat</code>（AES-256-CBC + HMAC），重启自动恢复登录态。',
      'cookies.colDomain': '域名 (Domain)',
      'cookies.colCount': '携带 Cookie 数量',
      'cookies.howto.title': '如何同步浏览器登录态？',
      'cookies.howto.step1': '安装配套的 Chrome 插件：<code>chrome-extension/</code>（详见项目代码仓库）',
      'cookies.howto.step2': '在浏览器中正常登录目标网站，点击插件图标，勾选当前域名进行一键同步',
      'cookies.howto.step3': '同步完成后，列表将会实时刷新，seimi-render 在抓取这些域名时将自动携带有效身份',
      'cookies.empty': '暂无已同步的 cookie',
      'cookies.confirmClear': '确认清空当前会话的 cookie？\n（清空渲染服务内存 + 持久文件置空，重启后不恢复。\n不影响你浏览器本身的 cookie。）',
      'cookies.clearFail': '清空失败：',
      'cookies.confirmPurge': '⚠️ 确认永久删除本地加密存储的 cookie？\n\n这将直接删除 data/cookies.dat 文件，销毁所有已持久化的登录态，重启后无法恢复。\n（不影响你浏览器本身的 cookie。）',
      'cookies.purgeFail': '永久删除失败：',

      // MCP 配置页
      'mcp.token.title': '访问 Token',
      'mcp.token.protected': '● 已启用密码保护',
      'mcp.token.unprotected': '○ 未启用密码保护',
      'mcp.token.placeholder': '（未启用密码，无 token）',
      'mcp.token.copy': '复制',
      'mcp.token.descOn': '此 token 由启动密码确定性派生（sha256("seimi-render:" + 密码)），与密码一一映射：密码不变 token 不变，重启/换机器都不变。所有受保护接口需带 <code>Authorization: Bearer &lt;token&gt;</code> 头，或 URL 加 <code>?token=&lt;token&gt;</code>。HTTP / WebSocket / MCP 三个端口都用这个 token，浏览器插件、curl、MCP 客户端统一复用。',
      'mcp.token.descOff': '当前未设置 --password，所有接口可直接访问，无需 token。启动时加 --password <密码> / --password-file <文件> / 环境变量 SEIMI_PASSWORD 即可启用。',
      'mcp.title': 'MCP 接入配置',
      'mcp.intro': '将以下配置参数注入对应 AI Agent 的工具链，即可赋予模型实时渲染解析网页的能力。',
      'mcp.endpoint': '当前 MCP 服务 Endpoint',
      'mcp.claude': 'Claude Code',
      'mcp.claudeHint': '(位于 ~/.claude.json 或项目 .mcp.json)',
      'mcp.cursor': 'Cursor',
      'mcp.cursorHint': '(Settings → MCP Servers)',
      'mcp.httpTitle': 'HTTP 接口调用示例',
      'mcp.authHintOn': '当前启用了密码保护，配置已带 Authorization 头（MCP 接入层同样校验此 token）。',
      'mcp.authHintOff': '当前未启用密码保护，MCP 无需 token。',
      'mcp.bindNote': 'MCP 端口默认只监听 127.0.0.1；agent 需远程接入时，启动加 --host 0.0.0.0（此时务必配合密码，MCP 接入层会校验同一个 token）。',

      // API 示例（app.js curl 注释 + hint）
      'api.hintOn': '当前已启用密码保护，下方示例已带 Authorization 头。token 由密码确定性派生（密码不变 token 不变）。',
      'api.hintOff': '当前未启用密码保护，无需 Authorization 头。若启用了 --password，示例需补上 -H "Authorization: Bearer <token>"。',
      'api.tab.render': '渲染 /render',
      'api.tab.status': '状态 /status',
      'api.tab.cookies': 'Cookie /cookies',
      'api.tab.pdf': 'PDF /pdf',
      'api.tab.screenshot': '截图 /image',
      'api.tab.markdown': 'Markdown',

      // docs.js 文档页通用
      'docs.loading': '加载接口文档中...',
      'docs.group.http': 'HTTP REST',
      'docs.group.mcp': 'MCP 工具',
      'docs.group.ws': 'WebSocket',
      'docs.authRequired': '🔒 需鉴权',
      'docs.public': '公开',
      'docs.tryBtn': '试一下',
      'docs.noParams': '无参数',
      'docs.required': '必填',
      'docs.optional': '可选',
      'docs.colName': '名称',
      'docs.colType': '类型',
      'docs.colRequired': '必填',
      'docs.colDesc': '说明',
      'docs.section.params': '请求参数',
      'docs.section.example': '调用示例',
      'docs.section.exampleMcp': '调用示例 (MCP)',
      'docs.section.response': '响应示例',
      'docs.section.errors': '错误码',
      'docs.colStatus': '状态码',
      'docs.colMeaning': '含义',
      'docs.tryForm.title': '🧪 试一下',
      'docs.tryForm.completePath': '完整路径（含 query）',
      'docs.tryForm.send': '发送请求',
      'docs.tryForm.placeholder': '点击发送查看响应',
      'docs.tryForm.requesting': '请求中...',
      'docs.tryForm.sendFail': '❌ 请求失败: ',
      'docs.tryForm.truncated': '\n...(已截断)',
      'docs.tryForm.jsonParseFail': '❌ {name} JSON 解析失败: ',

      // 语言切换按钮
      'lang.toggle': 'EN', // 中文态显示「EN」表示可切到英文
      'lang.title': '切换语言 / Switch language',
    },

    en: {
      'common.loading': 'Loading…',
      'common.noData': 'No data',
      'common.noSuccessTasks': 'No successful tasks yet',
      'common.copy': 'Copy',
      'common.copied': 'Copied',
      'common.copyFail': 'Copy failed',
      'common.default': 'default',

      'brand.title': 'seimi-render Console',
      'brand.sub': 'Console',
      'conn.title': 'Connection status',

      'login.hint': 'Secure access verification',
      'login.placeholder': 'Enter admin password',
      'login.submit': 'Sign in',
      'login.wrongPwd': 'Wrong password',
      'login.connFail': 'Connection failed: ',

      'tab.stats': 'Runtime Stats',
      'tab.render': 'Render Lab',
      'tab.cookies': 'Cookie State',
      'tab.mcp': 'Config Samples',
      'tab.docs': 'API Docs',

      'stats.title': 'Render Load',
      'stats.realtime': '(live)',
      'stats.cards.uptime': 'Uptime',
      'stats.cards.total': 'Total requests',
      'stats.cards.success': 'Succeeded',
      'stats.cards.fail': 'Failed',
      'stats.cards.rate': 'Success rate',
      'stats.cards.throughput': 'Throughput req/s',
      'stats.latency.title': 'Latency Distribution',
      'stats.latency.subtitle': '(succeeded / ms)',
      'stats.outputs.title': 'Output Type Demand',
      'stats.outputs.screenshot': 'Screenshot',
      'stats.domains.title': 'Top Domains',
      'stats.domains.topN': 'Top {n} ({total} domains total)',
      'stats.domains.topNLimited': 'Top {n}',
      'stats.domains.colRank': '#',
      'stats.domains.colDomain': 'Target domain',
      'stats.domains.colTotal': 'Total',
      'stats.domains.colSuccess': 'Succeeded',
      'stats.domains.colFail': 'Failed',
      'stats.domains.colRate': 'Success rate',
      'stats.domains.sortHint': 'Click headers to sort',
      'stats.domains.empty': 'No data',
      'stats.env.title': 'Environment',
      'stats.env.copy': '📋 Copy',
      'stats.env.copied': 'Copied',
      'stats.env.copyFail': 'Copy failed',
      'stats.env.os': 'OS',
      'stats.env.kernel': 'Kernel',
      'stats.env.hostname': 'Hostname',
      'stats.env.cpu': 'CPU',
      'stats.env.coresPhys': 'physical',
      'stats.env.coresLogical': 'logical',
      'stats.env.memory': 'Total memory',
      'stats.env.gpuYes': 'Present',
      'stats.env.gpuNo': 'None (headless server)',
      'stats.env.qt': 'Qt version',
      'stats.env.build': 'Build',
      'stats.env.cpuUsage': 'CPU usage',
      'stats.env.rss': 'Memory usage',
      'stats.env.sampledAt': 'sampled at {ts}',

      'workload.title': 'Render Load',
      'workload.workerActive': 'Worker Active',
      'workload.queuePending': 'Queue Backlog',
      'workload.queuePeak': 'Queue Peak',
      'workload.active': 'active',
      'workload.poolNotStarted': 'Render pool not started',
      'workload.desc': 'Worker total is the concurrency slots at startup (<code>--concurrency</code>); active count is the number of tasks currently rendering. Queue backlog means submit rate exceeds render rate; peak (<code>peak_pending</code>) is the historical max backlog length, monotonically non-decreasing, used to assess historical load level.',

      'proxy.title': 'Network Proxy',
      'proxy.off': 'Off',
      'proxy.on': 'Direct',
      'proxy.socks5On': 'SOCKS5 proxy active',
      'proxy.httpOn': 'HTTP proxy active',
      'proxy.genericOn': 'Proxy active',
      'proxy.kv.mode': 'Mode',
      'proxy.kv.directMode': 'Direct (no upstream proxy)',
      'proxy.directDesc': 'All render requests reach target sites directly without an upstream proxy. To route through a proxy, add <code>--proxy</code> at startup, or call <code>POST /proxy</code> at runtime (takes effect immediately, no restart needed).',
      'proxy.kv.protocol': 'Protocol',
      'proxy.kv.host': 'Host',
      'proxy.kv.port': 'Port',
      'proxy.kv.user': 'Username',
      'proxy.kv.auth': 'Auth',
      'proxy.authSet': 'Credentials configured',
      'proxy.authNone': 'No auth',
      'proxy.desc': 'Proxy is delivered to the Chromium network stack at runtime via <code>QNetworkProxy::setApplicationProxy</code> (Qt WebEngine polls it; switches within seconds). In-flight requests at the switch moment may still use the old proxy; new requests use the new one. Password is never echoed back.',

      'render.url': 'Target URL',
      'render.settle': 'Settle delay (Settle MS)',
      'render.longpoll': 'Long-poll timeout (Long Poll MS)',
      'render.outputs': 'Output format',
      'render.outHtml': 'HTML structure',
      'render.outMd': 'Markdown text',
      'render.outPdf': 'Page PDF',
      'render.outShot': 'Screenshot',
      'render.shotFmt': 'Screenshot encoding',
      'render.shotFmtHint': '(only when screenshot is checked)',
      'render.shotFmt.auto': 'Auto',
      'render.shotFmt.autoHint': '(smart pick by image ratio)',
      'render.shotFmt.png': 'PNG',
      'render.shotFmt.pngHint': '(lossless)',
      'render.shotFmt.jpg': 'JPEG',
      'render.shotFmt.jpgHint': '(smaller for photo pages)',
      'render.mdAlg': 'Markdown body algorithm',
      'render.mdAlgHint': '(only when Markdown is checked)',
      'render.mdAlg.conservative': 'Conservative',
      'render.mdAlg.conservativeHint': '(default, zero false-drop)',
      'render.mdAlg.readability': 'Readability',
      'render.mdAlg.readabilityHint': '(body extraction, drops non-body)',
      'render.extract': 'Site-specific extraction',
      'render.extractHint': '(structured SERP extraction, mutually exclusive with Markdown)',
      'render.extract.off': 'Off',
      'render.extract.offHint': '(default)',
      'render.extract.baidu': 'Baidu SERP',
      'render.extract.baiduHint': '(ad-removed, structured)',
      'render.extract.bing': 'Bing SERP',
      'render.extract.bingHint': '(structured)',
      'render.extract.google': 'Google SERP',
      'render.extract.googleHint': '(structured)',
      'render.submit': 'Submit Render Task',
      'render.submitting': 'Rendering…',
      'render.btn': 'Render',
      'render.copyCurl': 'Copy curl',
      'render.curlDone': 'curl copied',
      'render.curlFail': 'Copy curl failed',
      'render.status.working': 'Rendering…',
      'render.status.ok': '✓ Render succeeded ({ms} ms, task_id: {id})',
      'render.status.fail': '✗ {state}: {err} ({ms} ms)',
      'render.status.failed': 'render failed',
      'render.status.incomplete': 'incomplete',
      'render.status.reqFail': 'Request failed: ',
      'render.placeholder': 'Waiting to render…',
      'render.noOutput': 'No output',
      'render.pdfDownload': '⬇ Download PDF ({bytes} bytes)',
      'render.shotPreview': 'Screenshot preview ({fmt}, {bytes} bytes{note})',
      'render.shotNote.jpeg': '(optimized for photo pages)',
      'render.shotNote.lossless': '(lossless)',
      'render.shotDownload': '⬇ Download {fmt} ({bytes} bytes)',
      'render.serp.blocked': '⚠ Anti-bot block',
      'render.serp.blockedDesc': 'The search engine returned a verification page (not real results). Retry later or switch engine.',
      'render.serp.results': 'Search results ({n} items{ads})',
      'render.serp.adsFiltered': ', {n} ads removed',
      'render.serp.redirect': '(redirect)',
      'render.serp.rawJson': 'View raw JSON',
      'render.htmlPreview': 'HTML Preview',
      'render.mdLabel': 'Markdown',

      'cookies.title': 'Synced Cookies Overview',
      'cookies.desc': 'Login-state credentials held by the server for rendering (only domain and count shown; no sensitive plaintext).',
      'cookies.clear': 'Clear session',
      'cookies.purge': 'Delete permanently',
      'cookies.encrypted': '🔒 Encrypted & persisted to <code>data/cookies.dat</code> (AES-256-CBC + HMAC); login state auto-restored on restart.',
      'cookies.colDomain': 'Domain',
      'cookies.colCount': 'Cookie count',
      'cookies.howto.title': 'How to sync browser login state?',
      'cookies.howto.step1': 'Install the companion Chrome extension: <code>chrome-extension/</code> (see the project repo).',
      'cookies.howto.step2': 'Log in to the target site in your browser, click the extension icon, and check the current domain to sync.',
      'cookies.howto.step3': 'After sync, the list refreshes in real time; seimi-render will automatically carry valid identity when crawling these domains.',
      'cookies.empty': 'No synced cookies yet',
      'cookies.confirmClear': 'Clear cookies for the current session?\n(Erases render-service memory + nulls the persisted file; not restored after restart.\nDoes NOT affect your browser\'s own cookies.)',
      'cookies.clearFail': 'Clear failed: ',
      'cookies.confirmPurge': '⚠️ Permanently delete the locally encrypted cookie store?\n\nThis deletes data/cookies.dat directly, destroying all persisted login state; not recoverable after restart.\n(Does NOT affect your browser\'s own cookies.)',
      'cookies.purgeFail': 'Permanent delete failed: ',

      'mcp.token.title': 'Access Token',
      'mcp.token.protected': '● Password protection enabled',
      'mcp.token.unprotected': '○ No password protection',
      'mcp.token.placeholder': '(no password set, no token)',
      'mcp.token.copy': 'Copy',
      'mcp.token.descOn': 'This token is deterministically derived from the startup password (sha256("seimi-render:" + password)); it maps 1:1 to the password: same password → same token, stable across restarts/machines. All protected endpoints require an <code>Authorization: Bearer &lt;token&gt;</code> header, or a <code>?token=&lt;token&gt;</code> URL param. HTTP / WebSocket / MCP ports share this one token; browser extension, curl, and MCP clients all reuse it.',
      'mcp.token.descOff': 'No --password set; all endpoints are directly accessible without a token. Add --password <pwd> / --password-file <file> / env SEIMI_PASSWORD at startup to enable.',
      'mcp.title': 'MCP Integration Config',
      'mcp.intro': 'Inject the following config into your AI agent\'s toolchain to give the model real-time web render & parse capability.',
      'mcp.endpoint': 'Current MCP service endpoint',
      'mcp.claude': 'Claude Code',
      'mcp.claudeHint': '(in ~/.claude.json or project .mcp.json)',
      'mcp.cursor': 'Cursor',
      'mcp.cursorHint': '(Settings → MCP Servers)',
      'mcp.httpTitle': 'HTTP API Call Examples',
      'mcp.authHintOn': 'Password protection is enabled; the config includes an Authorization header (the MCP layer checks the same token).',
      'mcp.authHintOff': 'No password protection; MCP needs no token.',
      'mcp.bindNote': 'The MCP port listens on 127.0.0.1 by default; for remote agent access, start with --host 0.0.0.0 (pair with a password — the MCP layer checks the same token).',

      'api.hintOn': 'Password protection is enabled; the example below includes an Authorization header. The token is deterministically derived from the password (same password → same token).',
      'api.hintOff': 'No password protection; no Authorization header needed. If you enable --password, add -H "Authorization: Bearer <token>" to the examples.',
      'api.tab.render': 'Render /render',
      'api.tab.status': 'Status /status',
      'api.tab.cookies': 'Cookie /cookies',
      'api.tab.pdf': 'PDF /pdf',
      'api.tab.screenshot': 'Screenshot /image',
      'api.tab.markdown': 'Markdown',

      'docs.loading': 'Loading API docs…',
      'docs.group.http': 'HTTP REST',
      'docs.group.mcp': 'MCP Tools',
      'docs.group.ws': 'WebSocket',
      'docs.authRequired': '🔒 Auth required',
      'docs.public': 'Public',
      'docs.tryBtn': 'Try it',
      'docs.noParams': 'No parameters',
      'docs.required': 'required',
      'docs.optional': 'optional',
      'docs.colName': 'Name',
      'docs.colType': 'Type',
      'docs.colRequired': 'Required',
      'docs.colDesc': 'Description',
      'docs.section.params': 'Request Parameters',
      'docs.section.example': 'Call Example',
      'docs.section.exampleMcp': 'Call Example (MCP)',
      'docs.section.response': 'Response Example',
      'docs.section.errors': 'Error Codes',
      'docs.colStatus': 'Status',
      'docs.colMeaning': 'Meaning',
      'docs.tryForm.title': '🧪 Try it',
      'docs.tryForm.completePath': 'Full path (incl. query)',
      'docs.tryForm.send': 'Send Request',
      'docs.tryForm.placeholder': 'Click send to see the response',
      'docs.tryForm.requesting': 'Requesting…',
      'docs.tryForm.sendFail': '❌ Request failed: ',
      'docs.tryForm.truncated': '\n…(truncated)',
      'docs.tryForm.jsonParseFail': '❌ {name} JSON parse failed: ',

      'lang.toggle': '中', // 英文态显示「中」表示可切到中文
      'lang.title': '切换语言 / Switch language',
    },
  };

  // ====== 当前语言 ======
  // 首次读取：localStorage → 默认 zh。允许的值只有 'zh' | 'en'。
  let currentLang = (() => {
    const saved = localStorage.getItem(STORAGE_KEY);
    return (saved === 'en' || saved === 'zh') ? saved : 'zh';
  })();

  // ====== 取文本 ======
  // t('stats.cards.total') → 当前语言对应文本
  // 支持 {placeholder} 插值：t('render.status.ok', { ms: 123, id: 'abc' })
  function t(key, vars) {
    const dict = DICT[currentLang] || DICT.zh;
    let s = dict[key];
    if (s === undefined) s = DICT.zh[key];   // 缺英文回退中文
    if (s === undefined) return key;         // 都没有回退 key 本身（便于发现遗漏）
    if (vars) {
      for (const k in vars) {
        s = s.split('{' + k + '}').join(String(vars[k]));
      }
    }
    return s;
  }

  // ====== 应用到 DOM ======
  // 遍历带 data-i18n* 属性的元素，按属性类型写文本。
  // 静态 HTML 在 init 时调一次；切换语言时再调一次（配合动态部分的重渲染）。
  function applyI18n(root) {
    const scope = root || document;

    // textContent：纯文本
    scope.querySelectorAll('[data-i18n]').forEach(el => {
      const key = el.getAttribute('data-i18n');
      if (key) el.textContent = t(key);
    });

    // innerHTML：含 <code> 等标签的文案（与现有写法一致，文案来自受信字典，无注入风险）
    scope.querySelectorAll('[data-i18n-html]').forEach(el => {
      const key = el.getAttribute('data-i18n-html');
      if (key) el.innerHTML = t(key);
    });

    // placeholder：input/textarea 占位符
    scope.querySelectorAll('[data-i18n-placeholder]').forEach(el => {
      const key = el.getAttribute('data-i18n-placeholder');
      if (key) el.placeholder = t(key);
    });

    // <html lang> 跟随语言（语义/无障碍）
    document.documentElement.lang = (currentLang === 'en') ? 'en' : 'zh-CN';
    // <title>
    const titleEl = document.querySelector('title[data-i18n]');
    if (titleEl) document.title = t('brand.title');
  }

  // ====== 切换语言 ======
  // 存 localStorage + 更新当前态 + 应用到 DOM。
  // 动态部分（stats/render/docs 等）由 app.js 监听 i18n 事件后重渲染。
  function setLang(lang) {
    if (lang !== 'zh' && lang !== 'en') return;
    if (lang === currentLang) return;
    currentLang = lang;
    localStorage.setItem(STORAGE_KEY, lang);
    applyI18n();
    // 通知 app.js 重渲染动态部分（app.js 注册回调）
    if (window.__i18nListeners) {
      window.__i18nListeners.forEach(fn => { try { fn(lang); } catch (e) { console.error(e); } });
    }
  }

  function getCurrent() { return currentLang; }
  function toggle() { setLang(currentLang === 'zh' ? 'en' : 'zh'); }

  function onLanguageChange(fn) {
    if (!window.__i18nListeners) window.__i18nListeners = [];
    window.__i18nListeners.push(fn);
  }

  // 暴露给 app.js / docs.js
  window.i18n = { t, applyI18n, setLang, toggle, getCurrent, onLanguageChange };
})();
