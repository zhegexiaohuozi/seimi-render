// seimi-render 接口文档页
// 数据驱动：把全部接口（HTTP REST / MCP / WebSocket）定义成纯前端数据结构，
// 渲染成左侧分组导航树 + 右侧详情。符合 CSP（script-src 'self'，无内联脚本依赖）。
// "试一下"仅对 HTTP REST 开放（同源，满足 connect-src 'self'）；MCP/WS 给规范 + 示例。
//
// i18n：用户可见的文本字段（title/desc/params[].desc/errors[].msg）以 {zh,en} 双写，
// 渲染时按当前语言取（tr()）。固定 UI 文案经 window.i18n.t()。i18n.js 在本文件之前加载。
//
// 注意：本文件不重复声明 t —— app.js 已在共享全局作用域声明 const t = (k,v)=>window.i18n.t(k,v)，
// 且 docs.js 在 app.js 之后加载，这里再声明会触发 SyntaxError（重复的词法声明）导致整个文件解析失败。
// 取 {zh,en} 双写字段的当前语言值；非对象（如已是字符串）原样返回。
function tr(v) {
  if (v && typeof v === 'object' && (v.zh !== undefined || v.en !== undefined)) {
    const lang = window.i18n.getCurrent();
    return v[lang] !== undefined ? v[lang] : v.zh;
  }
  return v;
}

// ====== 接口规格数据（与 HttpServer/WsServer/McpServer 路由一一对应）======
const API_SPEC = {
  http: [
    {
      method: 'POST', path: '/render', auth: true, tryable: true,
      title: { zh: '提交渲染任务', en: 'Submit Render Task' },
      desc: {
        zh: '提交 URL 进行 Chromium 渲染。可设置 JS 等待时间、输出格式（HTML/Markdown/PDF/截图）、长轮询直接取结果。目标 URL 需通过 SSRF 校验（拦截内网/回环/链路本地/云元数据地址）。',
        en: 'Submit a URL for Chromium rendering. Options include JS wait time, output format (HTML/Markdown/PDF/screenshot), and long-poll to fetch the result directly. The target URL must pass SSRF validation (blocks intranet/loopback/link-local/cloud-metadata addresses).',
      },
      params: [
        { name: 'url', type: 'string', required: true, desc: { zh: '目标 http(s) URL', en: 'Target http(s) URL' } },
        { name: 'settle_ms', type: 'int', required: false, def: '2000', desc: { zh: 'loadFinished 后等待 JS 执行的毫秒数，范围 0-30000', en: 'Milliseconds to wait for JS after loadFinished; range 0-30000' } },
        { name: 'long_poll_ms', type: 'int', required: false, def: '0', desc: { zh: '>0 则阻塞等待渲染完成直接返回结果，上限 60000', en: '>0 blocks until render completes and returns the result directly; max 60000' } },
        { name: 'output', type: 'string|array', required: false, def: 'html', desc: { zh: '输出格式：html,markdown(md),pdf,screenshot(image,png,screenshot_png)', en: 'Output format: html, markdown (md), pdf, screenshot (image, png, screenshot_png)' } },
        { name: 'format', type: 'string', required: false, def: 'auto', desc: { zh: '截图编码：png/jpg/jpeg/auto（按图片占比智能选择）', en: 'Screenshot encoding: png/jpg/jpeg/auto (smart pick by image ratio)' } },
        { name: 'md_algorithm', type: 'string', required: false, def: 'conservative', desc: { zh: 'Markdown 正文算法：conservative(零误伤) / readability(正文定位)', en: 'Markdown body algorithm: conservative (zero false-drop) / readability (body extraction)' } },
        { name: 'extract', type: 'string', required: false, def: 'none', desc: { zh: '站点特定结构化提取：baidu_serp(百度去广告) / bing_serp(必应) / google_serp(Google)。与 md_algorithm 互斥，设为非 none 时不转 markdown', en: 'Site-specific structured extraction: baidu_serp (Baidu, ad-removed) / bing_serp (Bing) / google_serp (Google). Mutually exclusive with md_algorithm; when set to non-none, markdown is not produced' } },
      ],
      curl: `curl -X POST http://127.0.0.1:8088/render \\
  -H "Authorization: Bearer <token>" \\
  -H "Content-Type: application/json" \\
  -d '{"url":"https://example.com/","output":"markdown","long_poll_ms":30000}'`,
      example: { task_id: 'abc123', url: 'https://example.com/', state: 'succeeded', elapsed_ms: 2341, markdown: '# Example Domain\n...' },
      errors: [{ code: 400, msg: 'invalid json body / url required' }, { code: 403, msg: 'SSRF blocked: ...' }, { code: 401, msg: 'unauthorized' }],
    },
    {
      method: 'GET', path: '/status', auth: true, tryable: true,
      title: { zh: '运行时状态全景', en: 'Runtime Status Overview' },
      desc: {
        zh: '返回进程信息、累计计数、成功率、延迟分布(p50/p90/p99)、吞吐、输出类型分布、域名分布 Top-N、当前队列快照。',
        en: 'Returns process info, cumulative counters, success rate, latency distribution (p50/p90/p99), throughput, output-type distribution, Top-N domains, and the current queue snapshot.',
      },
      params: [
        { name: 'domains', type: 'int', required: false, def: '20', desc: { zh: '返回的域名条数，上限 200', en: 'Number of domains to return; max 200' } },
      ],
      curl: `curl -H "Authorization: Bearer <token>" \\
  "http://127.0.0.1:8088/status?domains=50"`,
      example: { uptime_human: '1d 02:03:04', totals: { requests: 1280, succeeded: 1267, failed: 13, success_rate: 0.989 }, latency_ms: { p50: 1820, p90: 3210, p99: 8800 } },
      errors: [{ code: 401, msg: 'unauthorized' }],
    },
    {
      method: 'GET', path: '/status/:id', auth: true, tryable: false,
      title: { zh: '任务状态查询（非阻塞）', en: 'Task Status Query (non-blocking)' },
      desc: {
        zh: '按 task_id 查询单个任务的当前状态。非阻塞，立即返回。不含 html/markdown 内容。',
        en: 'Query the current state of a single task by task_id. Non-blocking, returns immediately. Does not include html/markdown content.',
      },
      params: [
        { name: ':id', type: 'path', required: true, desc: { zh: '任务 ID（POST /render 返回的 task_id）', en: 'Task ID (the task_id returned by POST /render)' } },
      ],
      curl: `curl -H "Authorization: Bearer <token>" \\
  http://127.0.0.1:8088/status/abc123`,
      example: { task_id: 'abc123', url: 'https://example.com/', state: 'succeeded', elapsed_ms: 2341 },
      errors: [{ code: 404, msg: 'task not found' }, { code: 401, msg: 'unauthorized' }],
    },
    {
      method: 'GET', path: '/result/:id', auth: true, tryable: false,
      title: { zh: '拉取渲染结果（长轮询）', en: 'Fetch Render Result (long-poll)' },
      desc: {
        zh: '长轮询阻塞拉取任务结果。任务完成立即返回；未完成则等待至超时。支持指定想要的输出类型。',
        en: 'Long-poll blocking fetch of a task result. Returns immediately when the task is done; otherwise waits until timeout. Supports specifying the desired output type.',
      },
      params: [
        { name: ':id', type: 'path', required: true, desc: { zh: '任务 ID', en: 'Task ID' } },
        { name: 'output', type: 'string', required: false, def: 'html', desc: { zh: '想要的结果类型：html/markdown/md/pdf/screenshot/image', en: 'Desired result type: html/markdown/md/pdf/screenshot/image' } },
        { name: 'timeout', type: 'int', required: false, def: '25', desc: { zh: '长轮询超时秒数，上限 60', en: 'Long-poll timeout in seconds; max 60' } },
      ],
      curl: `curl -H "Authorization: Bearer <token>" \\
  "http://127.0.0.1:8088/result/abc123?output=markdown&timeout=30"`,
      example: { task_id: 'abc123', state: 'succeeded', markdown: '# Example Domain\n...', md_algorithm_used: 'conservative' },
      errors: [{ code: 404, msg: 'task not found' }, { code: 401, msg: 'unauthorized' }],
    },
    {
      method: 'GET', path: '/pdf/:id', auth: true, tryable: false,
      title: { zh: '下载 PDF', en: 'Download PDF' },
      desc: {
        zh: '下载任务的 PDF 渲染结果（二进制流）。Content-Type: application/pdf。需在 /render 时 output 含 pdf。',
        en: 'Download a task\'s PDF render result (binary stream). Content-Type: application/pdf. The /render call must have included pdf in output.',
      },
      params: [{ name: ':id', type: 'path', required: true, desc: { zh: '任务 ID', en: 'Task ID' } }],
      curl: `curl -H "Authorization: Bearer <token>" \\
  http://127.0.0.1:8088/pdf/abc123 -o page.pdf`,
      example: '(binary: application/pdf)',
      errors: [{ code: 404, msg: 'task not found / no pdf' }, { code: 401, msg: 'unauthorized' }],
    },
    {
      method: 'GET', path: '/image/:id', auth: true, tryable: false,
      title: { zh: '下载截图', en: 'Download Screenshot' },
      desc: {
        zh: '下载任务的截图（二进制）。Content-Type 按 format 为 image/png 或 image/jpeg。需在 /render 时 output 含 screenshot。',
        en: 'Download a task\'s screenshot (binary). Content-Type is image/png or image/jpeg depending on format. The /render call must have included screenshot in output.',
      },
      params: [{ name: ':id', type: 'path', required: true, desc: { zh: '任务 ID', en: 'Task ID' } }],
      curl: `curl -H "Authorization: Bearer <token>" \\
  http://127.0.0.1:8088/image/abc123 -o shot.png`,
      example: '(binary: image/png or image/jpeg)',
      errors: [{ code: 404, msg: 'task not found / no image' }, { code: 401, msg: 'unauthorized' }],
    },
    {
      method: 'GET', path: '/stats', auth: true, tryable: true,
      title: { zh: '队列统计快照', en: 'Queue Stats Snapshot' },
      desc: {
        zh: '简单的队列计数快照（向后兼容）。/status 提供更详细的全景数据。',
        en: 'A simple queue counter snapshot (kept for backward compatibility). /status provides a fuller overview.',
      },
      params: [],
      curl: `curl -H "Authorization: Bearer <token>" http://127.0.0.1:8088/stats`,
      example: { total: 12, pending: 0, running: 1, done: 11 },
      errors: [{ code: 401, msg: 'unauthorized' }],
    },
    {
      method: 'POST', path: '/cookies', auth: true, tryable: true,
      title: { zh: '同步浏览器登录态', en: 'Sync Browser Login State' },
      desc: {
        zh: '批量同步 cookies（Chrome 插件用）。仅入内存缓冲，注入由 GUI 线程异步完成。单批上限 5000 条。',
        en: 'Sync cookies in bulk (used by the Chrome extension). Only enters an in-memory buffer; injection is done asynchronously by the GUI thread. Max 5000 entries per batch.',
      },
      params: [
        { name: 'cookies', type: 'array', required: true, desc: { zh: 'Cookie 对象数组，每项含 name/value/domain/path/hostOnly/secure/httpOnly/expirationDate', en: 'Array of cookie objects; each has name/value/domain/path/hostOnly/secure/httpOnly/expirationDate' } },
      ],
      curl: `curl -X POST http://127.0.0.1:8088/cookies \\
  -H "Authorization: Bearer <token>" \\
  -H "Content-Type: application/json" \\
  -d '{"cookies":[{"name":"sid","value":"abc","domain":"example.com","path":"/","secure":true}]}'`,
      example: { stored: 1, applied: true },
      errors: [{ code: 400, msg: "invalid json / 'cookies' array empty" }, { code: 401, msg: 'unauthorized' }],
    },
    {
      method: 'GET', path: '/cookies', auth: true, tryable: true,
      title: { zh: 'Cookie 概览', en: 'Cookie Overview' },
      desc: {
        zh: '返回已同步 cookie 的域名与数量概览。不含 value（防会话泄露）。',
        en: 'Returns a domain-and-count overview of synced cookies. Does not include value (to prevent session leakage).',
      },
      params: [],
      curl: `curl -H "Authorization: Bearer <token>" http://127.0.0.1:8088/cookies`,
      example: { total: 3, domains: [{ domain: 'example.com', count: 3 }] },
      errors: [{ code: 401, msg: 'unauthorized' }],
    },
    {
      method: 'DELETE', path: '/cookies', auth: true, tryable: false,
      title: { zh: '清空 Cookie', en: 'Clear Cookies' },
      desc: {
        zh: '清空 cookie。默认清当前会话；?permanent=1 永久删除 data/cookies.dat 文件。',
        en: 'Clears cookies. Defaults to the current session; ?permanent=1 permanently deletes the data/cookies.dat file.',
      },
      params: [
        { name: 'permanent', type: 'query', required: false, def: '0', desc: { zh: '=1 则永久删除持久化文件，重启不恢复', en: '=1 permanently deletes the persisted file; not restored on restart' } },
      ],
      curl: `curl -X DELETE -H "Authorization: Bearer <token>" \\
  "http://127.0.0.1:8088/cookies?permanent=1"`,
      example: { purged: true },
      errors: [{ code: 401, msg: 'unauthorized' }],
    },
    {
      method: 'POST', path: '/proxy', auth: true, tryable: true,
      title: { zh: '设置网络代理（热切换）', en: 'Set Network Proxy (hot-swap)' },
      desc: {
        zh: '动态更新 Chromium 的上游代理。基于本地转发桥，新连接立即生效，已建立的连接不中断。type=direct 表示直连（清除代理）。支持 HTTP 与 SOCKS5 两种协议类型。',
        en: 'Dynamically update the Chromium upstream proxy. Based on a local forwarding bridge: new connections take effect immediately; established connections are not interrupted. type=direct means direct connection (clears the proxy). Supports both HTTP and SOCKS5.',
      },
      params: [
        { name: 'type', type: 'string', required: true, desc: { zh: 'direct | http | socks5（socks 别名）', en: 'direct | http | socks5 (socks is an alias)' } },
        { name: 'host', type: 'string', required: false, desc: { zh: '上游代理主机（http/socks5 必填）', en: 'Upstream proxy host (required for http/socks5)' } },
        { name: 'port', type: 'int', required: false, desc: { zh: '上游代理端口 1-65535（http/socks5 必填）', en: 'Upstream proxy port 1-65535 (required for http/socks5)' } },
        { name: 'user', type: 'string', required: false, desc: { zh: '上游认证用户名（可选）', en: 'Upstream auth username (optional)' } },
        { name: 'pass', type: 'string', required: false, desc: { zh: '上游认证密码（可选，不回显）', en: 'Upstream auth password (optional, never echoed)' } },
      ],
      curl: `curl -X POST http://127.0.0.1:8088/proxy \\
  -H "Authorization: Bearer <token>" \\
  -H "Content-Type: application/json" \\
  -d '{"type":"socks5","host":"127.0.0.1","port":1080,"user":"u","pass":"p"}'`,
      example: { ok: true, type: 'socks5', host: '127.0.0.1', port: 1080 },
      errors: [{ code: 400, msg: "'type' must be direct|http|socks5 / host+port required" }, { code: 401, msg: 'unauthorized' }],
    },
    {
      method: 'GET', path: '/proxy', auth: true, tryable: true,
      title: { zh: '查询当前代理配置', en: 'Query Current Proxy Config' },
      desc: {
        zh: '返回当前上游代理配置快照。密码不回显（仅 password_set 布尔）。',
        en: 'Returns a snapshot of the current upstream proxy config. Password is never echoed (only a password_set boolean).',
      },
      params: [],
      curl: `curl -H "Authorization: Bearer <token>" http://127.0.0.1:8088/proxy`,
      example: { type: 'http', host: 'proxy.example.com', port: 8080, user: 'u', password_set: true },
      errors: [{ code: 401, msg: 'unauthorized' }],
    },
    {
      method: 'DELETE', path: '/proxy', auth: true, tryable: false,
      title: { zh: '清除代理（恢复直连）', en: 'Clear Proxy (restore direct)' },
      desc: {
        zh: '等价于 POST /proxy {type:"direct"}。新连接恢复直连目标。',
        en: 'Equivalent to POST /proxy {type:"direct"}. New connections go direct to the target again.',
      },
      params: [],
      curl: `curl -X DELETE -H "Authorization: Bearer <token>" http://127.0.0.1:8088/proxy`,
      example: { ok: true, type: 'direct' },
      errors: [{ code: 401, msg: 'unauthorized' }],
    },
    {
      method: 'POST', path: '/api/login', auth: false, tryable: false,
      title: { zh: '登录获取 Token', en: 'Login to Get Token' },
      desc: {
        zh: '提交管理密码换取确定性 token。仅启用密码时注册。带按 IP 限流（连续 10 次失败锁定 10 分钟）。token = sha256("seimi-render:" + password)，HTTP/WS/MCP 共用。',
        en: 'Submit the admin password for a deterministic token. Registered only when a password is enabled. IP-based rate-limited (10 consecutive failures lock for 10 minutes). token = sha256("seimi-render:" + password); shared by HTTP/WS/MCP.',
      },
      params: [{ name: 'password', type: 'string', required: true, desc: { zh: '管理密码', en: 'Admin password' } }],
      curl: `curl -X POST http://127.0.0.1:8088/api/login \\
  -H "Content-Type: application/json" \\
  -d '{"password":"your-password"}'`,
      example: { token: 'a1b2c3...(64 hex chars)' },
      errors: [{ code: 401, msg: 'invalid password' }, { code: 429, msg: 'too many failed attempts, locked' }],
    },
    {
      method: 'GET', path: '/health', auth: false, tryable: true,
      title: { zh: '健康检查', en: 'Health Check' },
      desc: {
        zh: '无需鉴权的健康探活端点。',
        en: 'Auth-free health probe endpoint.',
      },
      params: [],
      curl: `curl http://127.0.0.1:8088/health`,
      example: { service: 'seimi-render', status: 'ok' },
      errors: [],
    },
    {
      method: 'GET', path: '/auth-status', auth: false, tryable: true,
      title: { zh: '鉴权状态', en: 'Auth Status' },
      desc: {
        zh: '返回是否启用了密码保护。前端据此判断是否需要登录。',
        en: 'Returns whether password protection is enabled. The frontend uses this to decide whether login is required.',
      },
      params: [],
      curl: `curl http://127.0.0.1:8088/auth-status`,
      example: { password_enabled: true },
      errors: [],
    },
  ],
  mcp: [
    {
      method: 'TOOL', path: 'render_url', endpoint: 'http://<host>:8090/mcp', auth: true,
      title: { zh: '渲染 URL 并取结果', en: 'Render URL and Get Result' },
      desc: {
        zh: 'MCP 工具：提交 URL 渲染并长轮询等结果，一步返回。支持 markdown/html/pdf/screenshot，可逗号组合。截图以 image content 返回（agent 可直接显示），PDF 以 base64 文本返回。内部调用 POST /render 带 long_poll_ms。供 Claude Code 等 agent 工具调用。',
        en: 'MCP tool: submit a URL for render and long-poll for the result in one step. Supports markdown/html/pdf/screenshot, comma-combined. Screenshots return as image content (agent can display directly); PDF returns as base64 text. Internally calls POST /render with long_poll_ms. Intended for agents like Claude Code.',
      },
      params: [
        { name: 'url', type: 'string', required: true, desc: { zh: '目标 http(s) URL', en: 'Target http(s) URL' } },
        { name: 'output', type: 'string', required: false, def: 'markdown', desc: { zh: '输出格式，逗号分隔：markdown(默认) / html / pdf / screenshot。组合如 markdown,screenshot', en: 'Output format, comma-separated: markdown (default) / html / pdf / screenshot. Combine e.g. markdown,screenshot' } },
        { name: 'md_algorithm', type: 'string', required: false, def: 'conservative', desc: { zh: 'Markdown 算法：conservative(默认,零误伤) / readability(正文定位)', en: 'Markdown algorithm: conservative (default, zero false-drop) / readability (body extraction)' } },
        { name: 'format', type: 'string', required: false, def: 'auto', desc: { zh: '截图编码：auto(默认,智能选择) / png(无损) / jpg(照片体积小)。仅 screenshot 生效', en: 'Screenshot encoding: auto (default, smart) / png (lossless) / jpg (smaller for photos). Only affects screenshot' } },
        { name: 'settle_ms', type: 'int', required: false, def: '2500', desc: { zh: 'loadFinished 后等待 JS 的毫秒', en: 'Milliseconds to wait for JS after loadFinished' } },
        { name: 'timeout_ms', type: 'int', required: false, def: '30000', desc: { zh: '长轮询超时毫秒', en: 'Long-poll timeout in milliseconds' } },
      ],
      curl: `# Claude Code 配置（~/.claude.json）
{
  "mcpServers": {
    "seimi-render": {
      "url": "http://127.0.0.1:8090/mcp",
      "headers": { "Authorization": "Bearer <token>" }
    }
  }
}`,
      example: { content: [{ type: 'text', text: 'task_id=abc state=succeeded url=https://example.com\n\n# Example Domain\n...' }] },
      errors: [{ code: 401, msg: 'unauthorized (missing/invalid token)' }],
    },
    {
      method: 'TOOL', path: 'get_render_result', endpoint: 'http://<host>:8090/mcp', auth: true,
      title: { zh: '按 ID 取渲染结果', en: 'Get Render Result by ID' },
      desc: {
        zh: 'MCP 工具：按 task_id 拉取已有任务结果。用于 render_url 超时(state=running)后补取。支持 markdown/html/screenshot/pdf。内部调用 GET /result/:id。',
        en: 'MCP tool: fetch an existing task result by task_id. Used to recover results after render_url times out (state=running). Supports markdown/html/screenshot/pdf. Internally calls GET /result/:id.',
      },
      params: [
        { name: 'task_id', type: 'string', required: true, desc: { zh: '任务 ID（render_url 返回）', en: 'Task ID (returned by render_url)' } },
        { name: 'output', type: 'string', required: false, def: 'markdown', desc: { zh: '输出格式：markdown(默认) / html / screenshot / pdf', en: 'Output format: markdown (default) / html / screenshot / pdf' } },
        { name: 'timeout_ms', type: 'int', required: false, def: '5000', desc: { zh: '长轮询超时毫秒（任务未完成时等待）', en: 'Long-poll timeout in milliseconds (waits if the task is not done)' } },
      ],
      curl: '(通过 MCP 协议调用，非 HTTP)',
      example: { content: [{ type: 'text', text: '...' }] },
      errors: [{ code: 401, msg: 'unauthorized' }],
    },
    {
      method: 'TOOL', path: 'browser_search', endpoint: 'http://<host>:8090/mcp', auth: true,
      title: { zh: '关键词搜索网页', en: 'Search the Web by Keyword' },
      desc: {
        zh: 'MCP 工具：按关键词搜索，返回搜索结果列表（标题/URL/摘要）。用户想"查找/了解/调研"但未给具体 URL 时优先用此工具。自动构建搜索引擎 URL 并 URL-encode 查询词。engine=baidu/bing/google 时返回去广告的结构化结果（JSON 块 + 编号列表）；其它引擎返回整页 markdown。拿到结果后用 get_web_content 打开任一链接读全文。',
        en: 'MCP tool: search by keyword and return a result list (title/URL/snippet). Prefer this tool when the user wants to "find/learn/research" but gives no specific URL. Auto-builds the search-engine URL and URL-encodes the query. With engine=baidu/bing/google returns ad-removed structured results (JSON block + numbered list); other engines return the full page markdown. Open any result link with get_web_content to read the full article.',
      },
      params: [
        { name: 'query', type: 'string', required: true, desc: { zh: '搜索词（纯文本，无需 URL-encode）。把最重要的词放前面', en: 'Search terms (plain text, no need to URL-encode). Put the most important words first' } },
        { name: 'engine', type: 'string', required: false, def: 'google', desc: { zh: '搜索引擎：google(默认,结果最佳但反爬最强) / bing / baidu(中文最佳) / duckduckgo。google 超时/失败时换 bing 或 baidu', en: 'Search engine: google (default, best results but heaviest anti-bot) / bing / baidu (best for Chinese) / duckduckgo. On google timeout/failure, switch to bing or baidu' } },
        { name: 'settle_ms', type: 'int', required: false, def: '2500', desc: { zh: 'loadFinished 后等待 JS/ajax 的毫秒', en: 'Milliseconds to wait for JS/ajax after loadFinished' } },
        { name: 'timeout_ms', type: 'int', required: false, def: '45000', desc: { zh: '最大等待毫秒（慢/被拦引擎可调到 60000）', en: 'Max wait in milliseconds (raise to 60000 for slow/blocked engines)' } },
      ],
      curl: '(通过 MCP 协议调用，非 HTTP)',
      example: { content: [{ type: 'text', text: '[browser_search engine=baidu query="Python" count=8]\n\n```json\n[{"title":"Welcome to Python.org","url":"https://www.python.org/",...}]\n```\n\nResults:\n1. **Welcome to Python.org**\n   https://www.python.org/...' }] },
      errors: [{ code: 401, msg: 'unauthorized' }, { msg: 'BLOCKED: 搜索引擎返回验证页（重试或换引擎）' }],
    },
    {
      method: 'TOOL', path: 'get_web_content', endpoint: 'http://<host>:8090/mcp', auth: true,
      title: { zh: '读取单篇文章正文', en: 'Read a Single Article Body' },
      desc: {
        zh: 'MCP 工具：打开单个 URL，用 readability 算法提取正文为干净的 markdown（去导航/广告/侧栏）。用户给具体 URL 要"读这篇"，或 browser_search 拿到结果链接后想看全文时用此工具。非文章页（表格/仪表盘等需要完整 DOM 的）改用 render_url md_algorithm=conservative。',
        en: 'MCP tool: open a single URL and extract the body into clean markdown via the readability algorithm (strips nav/ads/sidebar). Use when the user gives a specific URL to "read this", or after browser_search when you want the full text of a result link. For non-article pages (tables/dashboards that need full DOM), use render_url md_algorithm=conservative instead.',
      },
      params: [
        { name: 'url', type: 'string', required: true, desc: { zh: '目标文章/页面 http(s) URL', en: 'Target article/page http(s) URL' } },
        { name: 'md_algorithm', type: 'string', required: false, def: 'readability', desc: { zh: '提取算法：readability(默认,仅正文,适合新闻/博客/论文) / conservative(全 DOM,零误伤,readability 丢内容时用)', en: 'Extraction algorithm: readability (default, body only, good for news/blog/papers) / conservative (full DOM, zero false-drop, use when readability drops content)' } },
        { name: 'settle_ms', type: 'int', required: false, def: '2500', desc: { zh: 'loadFinished 后等待 JS/ajax 的毫秒', en: 'Milliseconds to wait for JS/ajax after loadFinished' } },
        { name: 'timeout_ms', type: 'int', required: false, def: '45000', desc: { zh: '最大等待毫秒（慢站可调到 60000）', en: 'Max wait in milliseconds (raise to 60000 for slow sites)' } },
      ],
      curl: '(通过 MCP 协议调用，非 HTTP)',
      example: { content: [{ type: 'text', text: '[get_web_content url=https://example.com/article md_algorithm=readability]\n\n# Article Title\n\nArticle body text...' }] },
      errors: [{ code: 401, msg: 'unauthorized' }],
    },
  ],
  ws: [
    {
      method: 'ACTION', path: 'render', events: ['created', 'finished'], auth: true,
      title: { zh: '提交渲染任务（WS）', en: 'Submit Render Task (WS)' },
      desc: {
        zh: '通过 WebSocket 提交渲染任务。完成后服务端主动推送 finished 事件。鉴权通过连接 URL 的 ?token= 或首条 auth 消息。',
        en: 'Submit a render task over WebSocket. The server pushes a finished event on completion. Auth via ?token= on the connect URL, or the first auth message.',
      },
      params: [
        { name: 'action', type: 'string', required: true, desc: { zh: '固定 "render"', en: 'Fixed "render"' } },
        { name: 'url', type: 'string', required: true, desc: { zh: '目标 URL', en: 'Target URL' } },
        { name: 'settle_ms', type: 'int', required: false, desc: { zh: 'JS 等待毫秒', en: 'JS wait in milliseconds' } },
        { name: 'output', type: 'string', required: false, desc: { zh: '输出格式', en: 'Output format' } },
        { name: 'format', type: 'string', required: false, desc: { zh: '截图编码', en: 'Screenshot encoding' } },
        { name: 'md_algorithm', type: 'string', required: false, desc: { zh: 'Markdown 算法', en: 'Markdown algorithm' } },
      ],
      curl: `// 浏览器 JS
const ws = new WebSocket('ws://127.0.0.1:8089/?token=<token>');
ws.onopen = () => ws.send(JSON.stringify({
  action: 'render', url: 'https://example.com/', output: 'markdown'
}));
ws.onmessage = (e) => console.log(JSON.parse(e.data));`,
      example: { event: 'created', task_id: 'abc123', url: 'https://example.com/' },
      errors: [{ code: 1008, msg: 'unauthorized (connection closed)' }],
    },
    {
      method: 'ACTION', path: 'subscribe', events: ['subscribed', 'finished'], auth: true,
      title: { zh: '订阅任务完成推送', en: 'Subscribe to Task Completion Push' },
      desc: {
        zh: '订阅已有任务，任务完成时接收 finished 推送。',
        en: 'Subscribe to an existing task; receive a finished push when the task completes.',
      },
      params: [
        { name: 'action', type: 'string', required: true, desc: { zh: '固定 "subscribe"', en: 'Fixed "subscribe"' } },
        { name: 'task_id', type: 'string', required: true, desc: { zh: '要订阅的任务 ID', en: 'Task ID to subscribe to' } },
      ],
      curl: `ws.send(JSON.stringify({ action: 'subscribe', task_id: 'abc123' }));`,
      example: { event: 'subscribed', task_id: 'abc123' },
      errors: [{ code: 1008, msg: 'unauthorized' }],
    },
    {
      method: 'ACTION', path: 'auth', events: ['authorized', 'error'], auth: false,
      title: { zh: 'WebSocket 鉴权', en: 'WebSocket Auth' },
      desc: {
        zh: '若连接时未带 ?token=，可用首条 auth 消息鉴权。失败则连接被关闭（code 1008）。',
        en: 'If the connection did not carry ?token=, authenticate with the first auth message. On failure the connection is closed (code 1008).',
      },
      params: [
        { name: 'action', type: 'string', required: true, desc: { zh: '固定 "auth"', en: 'Fixed "auth"' } },
        { name: 'token', type: 'string', required: true, desc: { zh: '登录 token', en: 'Login token' } },
      ],
      curl: `ws.send(JSON.stringify({ action: 'auth', token: '<token>' }));`,
      example: { event: 'authorized' },
      errors: [{ code: 1008, msg: 'invalid token, connection closed' }],
    },
  ],
};

// ====== 渲染 ======
function renderDocs() {
  const nav = document.getElementById('docs-nav');
  const groups = [
    { key: 'http', label: t('docs.group.http'), icon: '🌐' },
    { key: 'mcp', label: t('docs.group.mcp'), icon: '🔌' },
    { key: 'ws', label: t('docs.group.ws'), icon: '⚡' },
  ];
  let navHtml = '';
  groups.forEach(g => {
    const items = API_SPEC[g.key];
    navHtml += `<div class="docs-group">
      <div class="docs-group-title">${g.icon} ${g.label} <span class="docs-count">${items.length}</span></div>`;
    items.forEach((it, idx) => {
      navHtml += `<div class="docs-item" data-group="${g.key}" data-idx="${idx}">
        <span class="m-badge m-${it.method.toLowerCase()}">${it.method}</span>
        <span class="docs-item-path">${it.path}</span>
      </div>`;
    });
    navHtml += `</div>`;
  });
  nav.innerHTML = navHtml;

  // 绑定点击
  nav.querySelectorAll('.docs-item').forEach(el => {
    el.addEventListener('click', () => {
      nav.querySelectorAll('.docs-item').forEach(x => x.classList.remove('active'));
      el.classList.add('active');
      const item = API_SPEC[el.dataset.group][parseInt(el.dataset.idx)];
      renderDocDetail(item, el.dataset.group);
    });
  });
  // 默认选中第一个
  const first = nav.querySelector('.docs-item');
  if (first) first.click();
}

function renderDocDetail(item, group) {
  const panel = document.getElementById('docs-detail');
  const methodCls = 'm-' + item.method.toLowerCase();
  const authBadge = item.auth
    ? `<span class="badge badge-auth">${t('docs.authRequired')}</span>`
    : `<span class="badge badge-open">${t('docs.public')}</span>`;
  const tryBtn = (group === 'http' && item.tryable)
    ? `<button class="btn-try" id="docs-try-btn">${t('docs.tryBtn')}</button>`
    : '';

  let paramsRows = '';
  if (item.params.length === 0) {
    paramsRows = `<tr><td colspan="4" class="muted" style="text-align:center;padding:16px">${t('docs.noParams')}</td></tr>`;
  } else {
    paramsRows = item.params.map(p => {
      const reqCell = p.required
        ? `<span class="req">${t('docs.required')}</span>`
        : (p.def ? `<span class="opt">${t('docs.optional')}</span><br><span class="muted small">${t('common.default')} ${p.def}</span>` : `<span class="opt">${t('docs.optional')}</span>`);
      return `
      <tr>
        <td class="mono">${p.name}</td>
        <td>${p.type}</td>
        <td>${reqCell}</td>
        <td>${tr(p.desc)}</td>
      </tr>`;
    }).join('');
  }

  const errorsRows = (item.errors && item.errors.length)
    ? item.errors.map(e => `<tr><td class="mono">${e.code}</td><td>${e.msg}</td></tr>`).join('')
    : `<tr><td colspan="2" class="muted" style="text-align:center;padding:12px">—</td></tr>`;

  const exampleStr = typeof item.example === 'string'
    ? item.example
    : JSON.stringify(item.example, null, 2);

  const exampleTitle = (group !== 'ws' && item.method === 'TOOL') ? t('docs.section.exampleMcp') : t('docs.section.example');

  panel.innerHTML = `
    <div class="docs-head">
      <div class="docs-head-line">
        <span class="m-badge ${methodCls}">${item.method}</span>
        <span class="docs-path mono">${item.path}</span>
        ${authBadge}
      </div>
      <h3 class="docs-title">${tr(item.title)}</h3>
      <p class="docs-desc">${tr(item.desc)}</p>
      ${tryBtn}
    </div>
    <div class="docs-section">
      <h4>${t('docs.section.params')}</h4>
      <table class="docs-table">
        <thead><tr><th>${t('docs.colName')}</th><th>${t('docs.colType')}</th><th>${t('docs.colRequired')}</th><th>${t('docs.colDesc')}</th></tr></thead>
        <tbody>${paramsRows}</tbody>
      </table>
    </div>
    ${group !== 'ws' ? `
    <div class="docs-section">
      <h4>${exampleTitle}</h4>
      <div class="code-block">
        <pre>${escHtml(item.curl)}</pre>
        <button class="btn-copy" data-copy-text="${encodeURIComponent(item.curl)}">${t('common.copy')}</button>
      </div>
    </div>` : ''}
    <div class="docs-section">
      <h4>${t('docs.section.response')}</h4>
      <div class="code-block">
        <pre>${escHtml(exampleStr)}</pre>
        <button class="btn-copy" data-copy-text="${encodeURIComponent(exampleStr)}">${t('common.copy')}</button>
      </div>
    </div>
    <div class="docs-section">
      <h4>${t('docs.section.errors')}</h4>
      <table class="docs-table">
        <thead><tr><th>${t('docs.colStatus')}</th><th>${t('docs.colMeaning')}</th></tr></thead>
        <tbody>${errorsRows}</tbody>
      </table>
    </div>
    <div id="docs-try-panel"></div>
  `;

  // 绑定复制按钮
  panel.querySelectorAll('.btn-copy').forEach(btn => {
    btn.addEventListener('click', () => {
      const text = decodeURIComponent(btn.dataset.copyText);
      navigator.clipboard.writeText(text).then(() => {
        const orig = btn.textContent;
        btn.textContent = t('common.copied');
        setTimeout(() => { btn.textContent = orig; }, 1200);
      });
    });
  });

  // 绑定"试一下"
  const tryBtnEl = document.getElementById('docs-try-btn');
  if (tryBtnEl) tryBtnEl.addEventListener('click', () => renderTryForm(item));
}

// "试一下"表单：根据 method 与 path 生成可填写的输入
function renderTryForm(item) {
  const panel = document.getElementById('docs-try-panel');
  const pathParams = item.params.filter(p => p.type === 'path');
  const queryParams = item.params.filter(p => p.type !== 'path' && item.method === 'GET');
  const bodyParams = item.params.filter(p => p.type !== 'path' && item.method !== 'GET' && item.method !== 'DELETE');

  let formHtml = `<div class="docs-try-form"><h4>${t('docs.tryForm.title')}</h4><div class="try-fields">`;
  pathParams.forEach(p => {
    formHtml += `<label class="try-field"><span>${p.name}${p.required ? ' *' : ''}</span>
      <input type="text" data-try="path" data-name="${p.name}" placeholder="${p.name}"></label>`;
  });
  queryParams.forEach(p => {
    formHtml += `<label class="try-field"><span>${p.name}${p.required ? ' *' : ''}</span>
      <input type="text" data-try="query" data-name="${p.name}" placeholder="${p.def || p.name}"></label>`;
  });
  bodyParams.forEach(p => {
    if (p.type === 'array') {
      formHtml += `<label class="try-field try-field-wide"><span>${p.name}${p.required ? ' *' : ''}</span>
        <textarea data-try="body-array" data-name="${p.name}" placeholder='[{"name":"..."}]' rows="3"></textarea></label>`;
    } else {
      formHtml += `<label class="try-field"><span>${p.name}${p.required ? ' *' : ''}</span>
        <input type="text" data-try="body" data-name="${p.name}" placeholder="${p.def || p.name}"></label>`;
    }
  });
  // GET/POST 通用：允许额外 query 或 path 直接编辑（无参数的接口给一个 url 编辑框）
  if (item.params.length === 0 && item.method === 'GET') {
    formHtml += `<label class="try-field try-field-wide"><span>${t('docs.tryForm.completePath')}</span>
      <input type="text" id="try-raw-path" value="${item.path}"></label>`;
  }
  formHtml += `</div>
    <button class="btn-send" id="try-send-btn">${t('docs.tryForm.send')}</button>
    <div id="try-result" class="try-result muted">${t('docs.tryForm.placeholder')}</div>
  </div>`;
  panel.innerHTML = formHtml;

  document.getElementById('try-send-btn').addEventListener('click', async () => {
    const resultEl = document.getElementById('try-result');
    resultEl.className = 'try-result';
    resultEl.textContent = t('docs.tryForm.requesting');

    // 组装 URL
    let url = item.path;
    document.querySelectorAll('[data-try="path"]').forEach(el => {
      if (el.value) url = url.replace(':' + el.dataset.name, encodeURIComponent(el.value));
    });
    // 组装 query
    const qs = [];
    document.querySelectorAll('[data-try="query"]').forEach(el => {
      if (el.value) qs.push(el.dataset.name + '=' + encodeURIComponent(el.value));
    });
    const rawPath = document.getElementById('try-raw-path');
    if (rawPath && rawPath.value) url = rawPath.value;
    if (qs.length) url += (url.includes('?') ? '&' : '?') + qs.join('&');

    // 组装 body
    const opts = { method: item.method, headers: {} };
    const token = sessionStorage.getItem('seimi_token');
    if (token) opts.headers['Authorization'] = 'Bearer ' + token;

    if (bodyParams.length > 0) {
      const body = {};
      let hasBody = false;
      // 按参数声明的类型序列化：int 型不能作为字符串发送。
      // 历史问题：port 声明为 int，但旧代码直接塞 el.value（字符串 "7890"），
      // 后端 Qt 的 QJsonValue::toInt() 对 JSON 字符串类型返回 0 → port=0 →
      // 触发 "host+port required" 校验失败（参数明明都填了却报缺 host/port）。
      const intFields = new Set(
        item.params.filter(p => p.type === 'int').map(p => p.name)
      );
      let parseErr = null;
      document.querySelectorAll('[data-try="body"]').forEach(el => {
        if (el.value !== '') {
          if (intFields.has(el.dataset.name)) {
            const n = Number(el.value);
            body[el.dataset.name] = Number.isFinite(n) ? n : el.value;
          } else {
            body[el.dataset.name] = el.value;
          }
          hasBody = true;
        }
      });
      document.querySelectorAll('[data-try="body-array"]').forEach(el => {
        if (el.value.trim()) {
          try { body[el.dataset.name] = JSON.parse(el.value); hasBody = true; }
          catch (e) { parseErr = t('docs.tryForm.jsonParseFail', { name: el.dataset.name }) + e.message; }
        }
      });
      if (parseErr) { resultEl.innerHTML = `<div class="try-status-err">${escHtml(parseErr)}</div>`; return; }
      if (hasBody) {
        opts.headers['Content-Type'] = 'application/json';
        opts.body = JSON.stringify(body);
      }
    }

    try {
      const resp = await fetch(url, opts);
      const statusCls = resp.ok ? 'try-status-ok' : 'try-status-err';
      const ct = resp.headers.get('content-type') || '';
      let bodyText;
      if (ct.includes('application/json')) {
        bodyText = JSON.stringify(await resp.json(), null, 2);
      } else {
        bodyText = await resp.text();
        if (bodyText.length > 5000) bodyText = bodyText.slice(0, 5000) + t('docs.tryForm.truncated');
      }
      resultEl.innerHTML = `<div class="${statusCls}">HTTP ${resp.status} ${resp.statusText}</div>
        <pre class="try-body">${escHtml(bodyText)}</pre>`;
    } catch (e) {
      resultEl.innerHTML = `<div class="try-status-err">${t('docs.tryForm.sendFail')}${escHtml(e.message)}</div>`;
    }
  });
}

// HTML 转义（防止示例文本含 < > 破坏 DOM）
function escHtml(s) {
  return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

// 暴露给 app.js 在页签切换时调用
window.renderDocs = renderDocs;
