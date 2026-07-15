// seimi-render 浏览器指纹统一伪装脚本。
// 策略：所有渲染实例伪装成同一个 Chrome on Windows 桌面环境，大隐隐于市。
// 注入：QWebEngineScript，InjectionPoint=DocumentCreation，WorldId=MainWorld。
//
// 经验：navigator.webdriver 的抹除只能由 Chromium flag
// --disable-blink-features=AutomationControlled 完成（原生层不产生标记）。
// 切勿在 JS 层用 defineProperty 覆盖 webdriver/permissions —— 反爬会检测
// 「属性描述符被篡改」，JS 覆盖反而暴露意图。JS 层只做不引发篡改检测的一致性补全。
// ============================================================
(function () {
  'use strict';

  if (window.__seimiStealthApplied) return;
  try { Object.defineProperty(window, '__seimiStealthApplied', { value: true, writable: false }); } catch (e) {}

  var UA_PLATFORM = 'Win32';
  var HW_CONCURRENCY = 8;
  var DEVICE_MEMORY = 8;
  var LANGUAGES = ['zh-CN', 'zh', 'en'];
  var LANGUAGE = 'zh-CN';
  var SCREEN_W = 1920, SCREEN_H = 1080;
  var AVAIL_W = 1920, AVAIL_H = 1040;
  var COLOR_DEPTH = 24;
  var PIXEL_RATIO = 1;
  var WEBGL_VENDOR = 'Google Inc. (Google)';
  var WEBGL_RENDERER = 'ANGLE (Google, Vulkan 1.3.0 (SwiftShader Device), SwiftShader driver)';

  function defineGetter(obj, prop, getter) {
    try { Object.defineProperty(obj, prop, { get: getter, configurable: true }); } catch (e) {}
  }

  function defineValue(obj, prop, value) {
    try { Object.defineProperty(obj, prop, { value: value, configurable: true, writable: true }); } catch (e) {}
  }

  // native function 伪装：保留为 no-op。
  // 结论：chrome.runtime 方法 toString 的 native 伪装在当前反指纹对抗下是死结——
  // 修一个 lie（hasBadChromeRuntime）会制造更强 lie（hasToStringProxy）。
  // 故接受 hasBadChromeRuntime（软指标），换取不触发 hasToStringProxy（硬信号），
  // 仅保留 chrome.runtime 桩结构（结构存在性仍有意义）。
  function makeNative(fn, name) { return fn; }
  function nativize(obj, names) { /* no-op: 见上注释 */ }

  var nav = window.navigator;

  // —— 不碰 navigator.webdriver（由 Chromium flag 原生抹除，JS 覆盖会留篡改痕迹）——

  // platform / hardwareConcurrency / deviceMemory / language —— 一致性补全，不引发篡改检测
  defineGetter(nav, 'platform', function () { return UA_PLATFORM; });
  defineGetter(nav, 'hardwareConcurrency', function () { return HW_CONCURRENCY; });
  defineGetter(nav, 'deviceMemory', function () { return DEVICE_MEMORY; });
  defineGetter(nav, 'language', function () { return LANGUAGE; });
  defineGetter(nav, 'languages', function () { return LANGUAGES.slice(); });
  defineGetter(nav, 'maxTouchPoints', function () { return 0; });

  // navigator.vendor / productSub —— Chrome 桌面标准值
  defineGetter(nav, 'vendor', function () { return 'Google Inc.'; });
  defineGetter(nav, 'productSub', function () { return '20030107'; });
  // cookieEnabled —— 正常桌面浏览器默认为 true
  defineGetter(nav, 'cookieEnabled', function () { return true; });

  // userAgent：若含 QtWebEngine 字样，替换为纯 Chrome UA（与 HTTP 头对齐）
  var STEALTH_UA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/142.0.0.0 Safari/537.36';
  try {
    var realUA = nav.userAgent;
    if (/QtWebEngine/i.test(realUA)) {
      defineGetter(nav, 'userAgent', function () { return STEALTH_UA; });
      defineGetter(nav, 'appVersion', function () { return STEALTH_UA.replace('Mozilla/', ''); });
    }
  } catch (e) {}

  // —— Worker UA 覆盖：暂无可靠解法 ——
  // flag / setHttpUserAgent / JS Worker 构造器包装三条路径在 QtWebEngine 下全断
  //（flag 被丢弃、setHttpUserAgent 不覆盖 worker context、blob+importScripts 跨域失败）。
  // 文档层 UA 已统一 Chrome/142（HTTP 头 + navigator.userAgent），worker 层矛盾是已知残留。
  try {
    var uad = {
      brands: [
        { brand: 'Google Chrome', version: '142' },
        { brand: 'Chromium', version: '142' },
        { brand: 'Not_A Brand', version: '24' }
      ],
      mobile: false,
      platform: 'Windows',
      getHighEntropyValues: function () {
        return Promise.resolve({
          architecture: 'x86', bitness: '64', brands: this.brands,
          fullVersionList: [
            { brand: 'Google Chrome', version: '142.0.7444.110' },
            { brand: 'Chromium', version: '142.0.7444.110' },
            { brand: 'Not_A Brand', version: '24.0.0.0' }
          ],
          mobile: false, model: '', platform: 'Windows', platformVersion: '15.0.0',
          uaFullVersion: '142.0.7444.110'
        });
      },
      toJSON: function () { return { brands: this.brands, mobile: false, platform: 'Windows' }; }
    };
    defineGetter(nav, 'userAgentData', function () { return uad; });
  } catch (e) {}

  // navigator.plugins / mimeTypes —— Chrome 标配的 PDF Viewer
  try {
    function fakePlugin(name) {
      return { name: name, filename: 'internal-pdf-viewer', description: 'Portable Document Format', length: 1, 0: { type: 'application/pdf', suffixes: 'pdf', description: '' } };
    }
    var fplugs = [
      fakePlugin('PDF Viewer'), fakePlugin('Chrome PDF Viewer'),
      fakePlugin('Chromium PDF Viewer'), fakePlugin('Microsoft Edge PDF Viewer'),
      fakePlugin('WebKit built-in PDF')
    ];
    var pa = { length: fplugs.length };
    for (var i = 0; i < fplugs.length; i++) { pa[i] = fplugs[i]; pa[fplugs[i].name] = fplugs[i]; }
    pa.item = function (n) { return this[n] || null; };
    pa.namedItem = function (n) { return this[n] || null; };
    pa.refresh = function () {};
    defineGetter(nav, 'plugins', function () { return pa; });

    var fms = [
      { type: 'application/pdf', suffixes: 'pdf', description: 'Portable Document Format', enabledPlugin: fplugs[0] },
      { type: 'text/pdf', suffixes: 'pdf', description: 'Portable Document Format', enabledPlugin: fplugs[0] }
    ];
    var ma = { length: fms.length };
    for (var j = 0; j < fms.length; j++) { ma[j] = fms[j]; ma[fms[j].type] = fms[j]; }
    ma.item = function (n) { return this[n] || null; };
    ma.namedItem = function (n) { return this[n] || null; };
    defineGetter(nav, 'mimeTypes', function () { return ma; });
  } catch (e) {}

  // —— headless API 补全（CreepJS getLikesHeadless）——
  // 真实 Chrome 桌面这些 API 都存在；headless/Qt WebEngine 缺失会被判 like-headless。
  // 仅补存在性，不伪造行为（真 Chrome 在非安全上下文/无扩展时这些 API 也存在但调用受限）。

  // pdfViewerEnabled：真 Chrome 桌面 true，headless 常为 false。
  defineGetter(nav, 'pdfViewerEnabled', function () { return true; });

  // Web Share API（navigator.share / canShare）：真 Chrome 桌面存在。
  try {
    defineValue(nav, 'share', function () { return Promise.reject(new TypeError('navigator.share is not a function')); });
    defineValue(nav, 'canShare', function () { return false; });
    makeNative(nav.share, 'share'); makeNative(nav.canShare, 'canShare');
  } catch (e) {}

  // Contacts Manager API（navigator.contacts）：真 Chrome 桌面存在。
  try {
    defineGetter(nav, 'contacts', function () {
      return { select: function () { return Promise.reject(new DOMException('not allowed', 'SecurityError')); }, getProperties: function () { return Promise.resolve(['name','email','tel']); } };
    });
  } catch (e) {}

  // NetworkInformation（navigator.connection）：真 Chrome 桌面存在，含 downlinkMax。
  // Qt WebEngine offscreen 无真实网络栈感知，rtt 恒为 0 → creepjs 判 rtt 异常。
  // 故 rtt 为 0 或对象缺失时都须覆写。
  try {
    var hasConn = false, rttZero = false;
    try { hasConn = !!nav.connection; rttZero = (hasConn && nav.connection.rtt === 0); } catch (e2) {}
    if (!hasConn || rttZero) {
      defineGetter(nav, 'connection', function () {
        return { downlinkMax: Infinity, effectiveType: '4g', rtt: 50, downlink: 10, saveData: false, type: 'wifi', addEventListener: function () {}, removeEventListener: function () {}, dispatchEvent: function () { return true; } };
      });
    }
  } catch (e) {}

  // ServiceWorker（navigator.serviceWorker）：真 Chrome 桌面存在容器对象。
  try {
    if (!nav.serviceWorker) {
      defineGetter(nav, 'serviceWorker', function () {
        return { ready: Promise.resolve(), register: function () { return Promise.reject(new TypeError('registration failed')); }, getRegistration: function () { return Promise.resolve(undefined); }, getRegistrations: function () { return Promise.resolve([]); }, startMessages: function () {}, addEventListener: function () {}, removeEventListener: function () {}, controller: null };
      });
    }
  } catch (e) {}

	  // window.chrome —— Chrome 浏览器特征对象。creepjs 深度检测 chrome.runtime 的
	  // 原型链/属性描述符/API 行为，并对方法做 toString lie 检测。按真实 Chrome 无扩展场景模拟。
	  try {
	    if (!window.chrome) window.chrome = {};
	    // 匿名扩展 ID（32 位小写 hex），用于 getURL；无扩展页面 id 为 undefined
	    var EXTID = 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa';
	    // 模拟 Chrome Event 对象：有 addListener/removeListener/hasListener/hasListeners + rules API
	    function makeChromeEvent() {
	      var ev = {
		        addListener: function () {},
		        removeListener: function () {},
		        hasListener: function () { return false; },
		        hasListeners: function () { return false; },
		        addRules: function () {},
		        removeRules: function () {},
		        getRules: function () { return []; }
		      };
	      nativize(ev, ['addListener','removeListener','hasListener','hasListeners','addRules','removeRules','getRules']);
	      return ev;
	    }
			    if (!window.chrome.runtime) {
			      var runtime = {
			        OnInstalledReason: { install: 'install', update: 'update', chrome_update: 'chrome_update', shared_module_update: 'shared_module_update' },
			        PlatformArch: { arm: 'arm', x86_32: 'x86-32', x86_64: 'x86-64' },
			        PlatformOs: { mac: 'mac', win: 'win', android: 'android', cros: 'cros', linux: 'linux', openbsd: 'openbsd' },
			        RequestUpdateCheckStatus: { throttled: 'throttled', no_update: 'no_update', update_available: 'update_available' },
			        OnUpdateAvailableResult: { THROTTLED: 'THROTTLED', NO_UPDATE: 'NO_UPDATE', UPDATE_AVAILABLE: 'UPDATE_AVAILABLE' },
			        // connect / sendMessage：真实 Chrome 无扩展页面抛 "Could not establish connection..."
			        connect: function () { throw new Error('Could not establish connection. Receiving end does not exist.'); },
			        sendMessage: function () { throw new Error('Could not establish connection. Receiving end does not exist.'); },
			        // getURL: 基于哑扩展 ID 构造 chrome-extension:// URL
			        getURL: function (path) { return 'chrome-extension://' + EXTID + '/' + (path || ''); },
			        // 事件对象
			        onConnect: makeChromeEvent(),
			        onMessage: makeChromeEvent(),
			        onMessageExternal: makeChromeEvent(),
			        onInstalled: makeChromeEvent(),
			        onUpdateAvailable: makeChromeEvent(),
			        onStartup: makeChromeEvent(),
			        onSuspend: makeChromeEvent(),
			        onSuspendCanceled: makeChromeEvent(),
			        getManifest: function () {
			          return { manifest_version: 3, name: '', version: '', description: '' };
			        },
			        getBackgroundPage: function () { return null; },
			        openOptionsPage: function () {},
			        setUninstallURL: function () {},
			        reload: function () {}
			      };
			      nativize(runtime, ['connect','sendMessage','getURL','getManifest','getBackgroundPage','openOptionsPage','setUninstallURL','reload']);
			      window.chrome.runtime = runtime;
			      // 精确属性描述符：id/lastError 在真实 Chrome 中 configurable:false
			      try { Object.defineProperty(window.chrome.runtime, 'id',
			        { value: undefined, writable: false, enumerable: true, configurable: false }); } catch (e) {}
			      try { Object.defineProperty(window.chrome.runtime, 'lastError',
			        { value: undefined, writable: false, enumerable: true, configurable: false }); } catch (e) {}
			    }
    if (!window.chrome.app) {
      var app = { isInstalled: false, getDetails: function () { return null; }, getIsInstalled: function () { return false; }, InstallState: { DISABLED: 'disabled', INSTALLED: 'installed', NOT_INSTALLED: 'not_installed' }, RunningState: { CANNOT_RUN: 'cannot_run', READY_TO_RUN: 'ready_to_run', RUNNING: 'running' } };
      nativize(app, ['getDetails','getIsInstalled']);
      window.chrome.app = app;
    }
    // chrome.csi / chrome.loadTimes：真 Chrome 桌面无扩展页面也存在，creepjs 检查 chrome 键集合（缺失会暴露）。
    if (!window.chrome.csi) {
      window.chrome.csi = makeNative(function () { return { startE: Date.now(), onloadT: Date.now(), pageT: 0, tran: 15 }; }, 'csi');
    }
    if (!window.chrome.loadTimes) {
      window.chrome.loadTimes = makeNative(function () { return { commitLoadTime: Date.now()/1000, connectionInfo: 'h2', finishDocumentLoadTime: Date.now()/1000, finishLoadTime: Date.now()/1000, firstPaintAfterLoadTime: 0, firstPaintTime: Date.now()/1000, navigationType: 'Other', npnNegotiatedProtocol: 'h2', requestTime: Date.now()/1000, startLoadTime: Date.now()/1000, wasAlternateProtocolAvailable: false, wasFetchedViaSpdy: true, wasNpnNegotiated: true }; }, 'loadTimes');
    }
  } catch (e) {}

  // Notification API —— mock 为正常桌面 Chrome 的未授权状态
  try {
    if (typeof Notification !== 'undefined') {
      defineValue(Notification, 'permission', 'default');
      var origReqPerm = Notification.requestPermission;
      if (origReqPerm) {
        Notification.requestPermission = function (cb) {
          var p = Promise.resolve('default');
          if (typeof cb === 'function') { p.then(cb); }
          return p;
        };
      }
    }
  } catch (e) {}

  // 修复 Qt WebEngine 默认背景色（creepjs hasKnownBgColor 检测）。
  // creep.js 以 <script defer> 执行，须抢在它之前注入 CSS，故用 MutationObserver 监听 body 出现。
  try {
    var fixBg = function () {
      try {
        var s = document.createElement('style');
        s.textContent = 'html,body{background-color:rgb(255,255,255)!important}';
        (document.head || document.documentElement).appendChild(s);
      } catch (e) {}
      try { document.documentElement.style.backgroundColor = 'rgb(255, 255, 255)'; } catch (e) {}
    };
    // 立即尝试（DocumentCreation 时 documentElement 可能已存在）
    if (document.documentElement) { fixBg(); }
    // MutationObserver：body 出现时立即注入（先于 defer 脚本）
    var obs = new MutationObserver(function (ms) {
      for (var i = 0; i < ms.length; i++) {
        for (var j = 0; j < ms[i].addedNodes.length; j++) {
          if (ms[i].addedNodes[j].nodeName === 'BODY') { fixBg(); obs.disconnect(); return; }
        }
      }
    });
    obs.observe(document.documentElement || document, { childList: true, subtree: true });
    // setTimeout 兜底（极端情况）
    setTimeout(fixBg, 0);
  } catch (e) {}

  // —— 不覆盖 navigator.permissions.query（会留篡改痕迹，Google 会检测）——

  // screen / devicePixelRatio / window outer —— 统一尺寸指纹
  try {
    var scr = window.screen;
    defineGetter(scr, 'width', function () { return SCREEN_W; });
    defineGetter(scr, 'height', function () { return SCREEN_H; });
    defineGetter(scr, 'availWidth', function () { return AVAIL_W; });
    defineGetter(scr, 'availHeight', function () { return AVAIL_H; });
    defineGetter(scr, 'colorDepth', function () { return COLOR_DEPTH; });
    defineGetter(scr, 'pixelDepth', function () { return COLOR_DEPTH; });
    defineGetter(scr, 'availTop', function () { return 0; });
    defineGetter(scr, 'availLeft', function () { return 0; });
  } catch (e) {}
  defineGetter(window, 'devicePixelRatio', function () { return PIXEL_RATIO; });

  // window.outerWidth / outerHeight —— headless 默认 0 是经典检测信号，设为与 viewport 一致。
  defineGetter(window, 'outerWidth', function () { return SCREEN_W; });
  defineGetter(window, 'outerHeight', function () { return SCREEN_H; });

  // WebGL 指纹 —— 固定 vendor/renderer
  try {
    var VU = 0x9245, RU = 0x9246;
    function patchWebGL(proto) {
      if (!proto || !proto.getParameter) return;
      var orig = proto.getParameter;
      proto.getParameter = function (p) {
        if (p === VU) return WEBGL_VENDOR;
        if (p === RU) return WEBGL_RENDERER;
        return orig.call(this, p);
      };
    }
    if (window.WebGLRenderingContext) patchWebGL(window.WebGLRenderingContext.prototype);
    if (window.WebGL2RenderingContext) patchWebGL(window.WebGL2RenderingContext.prototype);
  } catch (e) {}

  // canvas / audio 指纹：不扰动。
  // 对 toDataURL/toBlob/getChannelData 注入噪声会被反爬归类为反指纹扩展（比 canvas
  // 哈希本身更可疑的 lie）。真实 Chrome 桌面 canvas/audio 哈希稳定、机器相关，不归一化；
  // 软件渲染下多实例本就趋同，天然满足融入人群。故暴露真实硬件指纹。

  // Intl.DateTimeFormat 时区标准化 —— 统一为 Asia/Shanghai，与 zh-CN locale 一致
  //（headless 可能暴露服务器真实时区如 UTC，与 zh-CN 矛盾是不一致信号）。
  try {
    if (typeof Intl !== 'undefined' && Intl.DateTimeFormat) {
      var origResolved = Intl.DateTimeFormat.prototype.resolvedOptions;
      Intl.DateTimeFormat.prototype.resolvedOptions = function () {
        var opts = origResolved.call(this);
        if (opts && opts.timeZone) {
          opts.timeZone = 'Asia/Shanghai';
        }
        return opts;
      };
    }
  } catch (e) {}

  // 抹除老式 headless 残留
  try { delete window.callPhantom; delete window._phantom; delete window.__nightmare; delete window.Buffer; } catch (e) {}
})();
