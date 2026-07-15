#pragma once

//
// 恒定时间字符串比较，用于 token / 密码等敏感凭据的相等判定。
//
// 普通 operator== / strcmp 一旦发现不匹配即返回，执行时间与公共前缀长度相关，
// 可被远程时序侧信道逐字节枚举。凭据比较一律走恒定时间实现消除该侧信道。
// 长度不同直接返回 false（token 长度固定，非敏感信息）；长度相同则逐字节 XOR 累加。
//
// 仅依赖标准库，便于在纯 C++（McpServer）与 Qt（HttpServer/WsServer）中复用。

#include <cstdint>
#include <string>

namespace seimi {

// 恒定时间比较两个 std::string。长度不同直接返回 false；长度相同则逐字节 XOR
// 累加，最终结果为 0 才相等。对相同长度的输入，循环次数恒定，不因匹配位置提前退出。
inline bool constantTimeEquals(const std::string& a, const std::string& b) {
    // 长度不同：直接返回。先记一份长度避免某些实现里 size() 调用本身有差异。
    const std::string::size_type la = a.size();
    const std::string::size_type lb = b.size();
    if (la != lb) return false;

    // 逐字节累积差异。注意：即便长度相同也遍历完整串，不提前 break。
    volatile std::uint8_t diff = 0;  // volatile 防止编译器把循环优化成短路
    for (std::string::size_type i = 0; i < la; ++i) {
        diff |= static_cast<std::uint8_t>(a[i]) ^ static_cast<std::uint8_t>(b[i]);
    }
    return diff == 0;
}

} // namespace seimi
