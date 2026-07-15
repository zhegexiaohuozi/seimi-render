# cpp-mcp (seimi vendored fork)

本目录是第三方库 **cpp-mcp** 的 vendored 副本，已从 git submodule 转为本仓库直接管理的源码，
以便持续迭代 seimi 的定制改动。

## 上游来源

- 仓库：https://github.com/hkr04/cpp-mcp
- 基线 commit：见 `.seimi-upstream-base`（当前为 `a0eb22c98dbd8ce8b3ef69679310c1a038905c08`）
- 许可证：MIT（见 `LICENSE`）

## seimi 定制改动（相对上游）

1. **接入层鉴权**（`src/mcp_server.cpp` + `include/mcp_server.h`）
   - 上游的 `set_auth_handler` 只是存储 handler、从不调用（注释自述 "Not used in the current implementation"）。
   - seimi 新增私有方法 `enforce_auth_(req, res)`，在 5 个 HTTP 入口（`handle_sse` / `handle_jsonrpc` /
     `handle_mcp_post` / `handle_mcp_get` / `handle_mcp_delete`）开头调用，校验
     `Authorization: Bearer <token>` 或 `?token=<token>`，未通过返回 401。
   - 由 `src/McpServer.cpp` 注入 handler，token 与 HTTP/WebSocket 三个端口共用。

## 与上游的差异范围

构建相关文件已精简，本目录只保留 seimi 实际编译所需的内容：

```
include/         cpp-mcp 全部公共头（含上述鉴权声明）
src/*.cpp        cpp-mcp 实现（含上述鉴权改动）
common/
  base64.hpp     cpp-mcp 依赖
  json.hpp       cpp-mcp 依赖（nlohmann/json）
LICENSE          上游许可证（保留）
.seimi-upstream-base   记录基线 commit，便于升级时 diff
```

已剔除（非 seimi 构建所需）：

- `common/httplib.h` —— 改用项目统一的 `third_party/httplib.h`（0.47），避免与 cpp-mcp 自带的旧版 ODR 冲突
- `examples/`、`test/`（含 googletest 子依赖）
- cpp-mcp 自己的 `CMakeLists.txt` —— seimi 在根 `CMakeLists.txt` 里直接编这些源码
- `src/CMakeLists.txt`（同上）

## 构建方式

seimi 不使用本目录的 CMakeLists，而是在仓库根 `CMakeLists.txt` 里定义独立的静态库目标
`seimi-mcp`，直接编译 `src/*.cpp`。include path 通过该目标的 `target_include_directories` 配置。
详见根 CMakeLists 的 `MCP SDK (hkr04/cpp-mcp)` 段。

## 升级上游流程

```bash
# 1. 在临时位置 clone 上游，checkout 到目标 commit/tag
git clone https://github.com/hkr04/cpp-mcp /tmp/cpp-mcp-upstream
cd /tmp/cpp-mcp-upstream
git checkout <new-tag-or-commit>

# 2. diff 本目录与上游，重点关注我们改过的两个文件
diff -u third_party/cpp-mcp/src/mcp_server.cpp  /tmp/cpp-mcp-upstream/src/mcp_server.cpp
diff -u third_party/cpp-mcp/include/mcp_server.h /tmp/cpp-mcp-upstream/include/mcp_server.h

# 3. 手动合并：把上游新改动应用到本目录，【保留】seimi 的 enforce_auth_ 相关代码
#    （搜索 "seimi patch" 标记可快速定位所有定制点）

# 4. 同步 common/base64.hpp、common/json.hpp（若上游升级了版本）

# 5. 更新 .seimi-upstream-base 为新 commit hash

# 6. 重新编译 + 运行鉴权冒烟测试（MCP 端口无 token 应 401，带 token 应通过）
```
