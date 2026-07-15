// SPDX-FileCopyrightText: 2026 wanghaomiao.cn
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QString>

#include <atomic>
#include <cstdint>

namespace seimi {

// 任务状态机：Pending -> Running -> Succeeded | Failed
enum class TaskState : std::uint8_t {
    Pending = 0,   // 已入队，等待渲染槽
    Running = 1,   // WebEngine 正在加载
    Succeeded = 2, // 渲染完成，结果（html/markdown/image 按请求的 output）可取
    Failed = 3,    // 加载失败/超时
};

inline const char* taskStateName(TaskState s) {
    switch (s) {
        case TaskState::Pending:   return "pending";
        case TaskState::Running:   return "running";
        case TaskState::Succeeded: return "succeeded";
        case TaskState::Failed:    return "failed";
    }
    return "unknown";
}

// 输出类型位标记：请求里用 output=html,markdown,pdf,screenshot 任意组合。
// HTML 总会被采集（markdown 依赖），仅在请求 html 时回传（省 JSON 体积）。
// screenshot 为真实像素截图（PNG），走 GET /image/:id 下载。
// 短页用视口重置法（resize 全高后 grab），超长页（>8000px）降级滚动拼接法。
enum class Output : std::uint8_t {
    Html       = 1 << 0,
    // 1 << 1 保留（历史跳过）
    Markdown   = 1 << 2,
    Pdf        = 1 << 3,
    Screenshot = 1 << 4,   // PNG 像素截图（区别于 pdf 的打印输出）
};

using OutputMask = std::uint8_t;

// 截图编码格式。auto 时按图片像素占比智能选择（>阈值用 JPEG，否则 PNG）。
enum class ImageFormat : std::uint8_t {
    Png,   // 无损，适合文字/线条类页面
    Jpeg,  // 有损 q85，适合照片密集页面，体积小
    Auto,  // 智能：JS 统计 img 像素占比决定（默认）
};

// markdown 正文提取算法。仅 wants(Markdown) 时有意义。
enum class MdAlgorithm : std::uint8_t {
    Conservative,  // 保守：DOM 简化（删 script/style/nav/iframe 等噪音）后转 markdown
    Readability,   // Mozilla Readability 正文定位：剔除导航/侧栏/版权，质量高但概率性
    Raw,           // 原始完整 DOM 直接转 markdown（Readability/Conservative 抽取失败时的兜底）
                   //   —— 仅作为实际算法（mdAlgorithmUsed）的取值，不可作为请求算法。
};

inline const char* mdAlgorithmName(MdAlgorithm a) {
    switch (a) {
        case MdAlgorithm::Conservative: return "conservative";
        case MdAlgorithm::Readability:  return "readability";
        case MdAlgorithm::Raw:           return "raw";
    }
    return "unknown";
}

// 站点特定结构化提取算法（与 MdAlgorithm 正交：MdAlgorithm 管 HTML→markdown 转换策略，
// ExtractAlgorithm 管"识别页面类型并结构化抽取"）。仅 extract != None 时有意义。
enum class ExtractAlgorithm : std::uint8_t {
    None,        // 默认：不做 SERP 提取（现有行为不变）
    BaiduSerp,   // 百度搜索结果页结构化提取（去广告，输出 results JSON）
    BingSerp,    // 必应搜索结果页结构化提取（输出 results JSON）
    GoogleSerp,  // Google 搜索结果页结构化提取（输出 results JSON）
    // 预留：DuckDuckGoSerp —— 框架就位后按需添加
};

inline const char* extractAlgorithmName(ExtractAlgorithm a) {
    switch (a) {
        case ExtractAlgorithm::None:       return "none";
        case ExtractAlgorithm::BaiduSerp:  return "baidu_serp";
        case ExtractAlgorithm::BingSerp:   return "bing_serp";
        case ExtractAlgorithm::GoogleSerp: return "google_serp";
    }
    return "unknown";
}


// 一个渲染任务的完整数据。只读字段在入队后不再修改；
// 可变字段（state/results/error/done）通过 RenderQueue 的锁保护。
struct RenderTask {
    // 永不变更
    const QString id;
    const QString url;
    const qint64 createdAtMsec;       // 提交时刻（epoch ms），由提交线程填入
    const int settleDelayMsec;        // loadFinished 后额外等待的 JS 执行时间
    const OutputMask outputs;         // 请求的输出类型位标记
    const ImageFormat imageFormat;    // 截图编码格式（仅 wants(Screenshot) 时有意义）
    const MdAlgorithm mdAlgorithm;    // markdown 正文提取算法（仅 wants(Markdown) 时有意义）
    const ExtractAlgorithm extractAlgorithm;  // 站点特定提取算法（默认 None，不影响现有行为）

    // 受 RenderQueue 锁保护的可变字段
    TaskState state{TaskState::Pending};
    QString html;                     // 渲染后的完整 HTML（成功时填；转 markdown 也依赖它）
    QString markdown;                 // HTML 转 markdown（请求了 markdown 时填）
    QByteArray pdfData;               // 页面打印 PDF（请求了 pdf 时填）
    QByteArray imageData;             // 页面截图（请求了 screenshot 时填，PNG 或 JPEG）
    ImageFormat resolvedImageFmt{ImageFormat::Png};  // 实际编码格式（供 /image 路由设 Content-Type）
    bool imageTruncated{false};       // 截图是否因超高度上限被截断（无限滚动页防护）
    // markdown 实际使用的算法（可观测性）：可能因 Readability 判定非文章页而回退 conservative。
    MdAlgorithm mdAlgorithmUsed{MdAlgorithm::Conservative};
    QString serpJson;                  // SERP 结构化提取结果 JSON 字符串（extract!=None 且成功时填）
    QString error;                    // 失败原因
    bool done{false};                 // 终态标志（Succeeded 或 Failed）

    // 计时（受锁保护），用于观测
    qint64 startedAtMsec{0};
    qint64 finishedAtMsec{0};

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

    bool wants(Output o) const { return (outputs & static_cast<OutputMask>(o)) != 0; }
};

} // namespace seimi
