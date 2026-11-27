# cliproxyapi-usage-metrics

[中文](README.md) | [English](README_EN.md)

面向 [CLIProxyAPI](https://github.com/router-for-me/CLIProxyAPI) 的轻量级、零外部依赖 C ABI（`usage_plugin`）用量与性能监控插件，内置实时 Web 仪表盘。

---

## 核心功能

- **纯 C 零依赖，单文件部署**：仅依赖标准 C 库与 POSIX 线程（`-pthread`），编译后仅约 83 KB 单个 `.so` 文件；前端页面在编译期通过 `gen_ui_header.py` 打包进 `ui_assets.h`，无需额外部署静态文件。
- **异步无侵入监听**：通过 CLIProxyAPI 原生 `usage_plugin` C ABI 钩子异步采集数据，不阻塞、不改写任何上游模型转发请求。
- **精细化 Token 与缓存统计**：
  - 统计总输入、未命中输入（`输入 - 缓存`）、缓存读取、正文输出与思考（Reasoning）Token。
  - 实时计算整体与单次请求的 **Prompt 缓存命中率**（`缓存读取 / 总输入`）及**命中次数**。
- **延迟与真实输出速度监控**：
  - 记录 **平均首字延迟（TTFT）** 及最小/最大耗时、端到端总耗时。
  - 区分 **流式** 与 **非流式** 请求，分别统计真实的解码吞吐速度（`token/s`，基于 `(输出 + 思考) / 生成耗时`）。
- **多维度汇总与明细检索**：
  - 支持按 **今日 / 近 7 天 / 全部** 时间窗口切换。
  - 支持 **按模型** 与 **按账号（`auth_id`）** 双维度汇总统计。
  - 最近请求明细支持分页浏览、按模型与账号联合筛选，以及一键导出 JSON 与清空统计。
- **原子落盘持久化**：内置 4,000 条环形明细缓冲区与全历史累计总账，每 5 秒或满 10 条记录自动通过“写临时文件 + 原子重命名”持久化到 `plugins/usage-metrics.state.json`，优雅退出时自动落盘，防止断电或重启丢数据。

---

## 环境要求与编译构建

需要 Linux 环境下的 `gcc` 与 `python3`（用于将 `ui.html` 生成为 C 头文件 `ui_assets.h`）。

```bash
git clone https://github.com/yangzhicong/cliproxyapi-usage-metrics.git
cd cliproxyapi-usage-metrics

# 方式一：使用构建脚本
./build.sh ./usage-metrics.so

# 方式二：使用 Makefile
make
```

默认开启 `-O2 -Wall -Wextra -Werror -pthread` 严格编译检查，编译成功后生成 `usage-metrics.so`。

---

## 部署与使用方法

1. **放入 CLIProxyAPI 插件目录**

```bash
cp usage-metrics.so /path/to/CLIProxyAPI/plugins/linux/amd64/usage-metrics.so
chmod 755 /path/to/CLIProxyAPI/plugins/linux/amd64/usage-metrics.so
```

2. **重启 CLIProxyAPI（或触发插件热重载）**，在日志中确认插件注册成功：

```text
pluginhost: plugin registered plugin_id=usage-metrics plugin_name=usage-metrics version=1.0.10
```

3. **打开 Web 监控面板或调用数据接口**

- **Web 监控面板**：`http://<你的CPA地址>/v0/resource/plugins/usage-metrics/ui`
  - 页面每 5 秒自动刷新，切换回标签页时也会立即刷新。
- **HTTP JSON 接口**：
  - 查询汇总与明细：`GET /v0/resource/plugins/usage-metrics/api?op=summary&range=today&limit=500`
    - `range` 可选：`today`（今日）、`7d`（近 7 天）、`all`（全部）
    - 支持附加筛选参数：`&model=<模型名>`、`&auth=<账号ID>`
  - 导出完整状态 JSON：`GET /v0/resource/plugins/usage-metrics/api?op=export`
  - 清空全部统计记录：`GET /v0/resource/plugins/usage-metrics/api?op=clear`

---

## 项目结构

```text
.
├── plugin.c          # C ABI v1 插件核心：JSON 解析/生成、环形缓冲、统计汇总与原子持久化
├── ui.html           # 单文件响应式 Web 监控面板（自动刷新、分页、按模型/账号筛选）
├── gen_ui_header.py  # 编译前将 ui.html 编码为 Base64 C 头文件（ui_assets.h）
├── build.sh          # 一键编译脚本
├── Makefile          # 标准 Make 构建配置
├── README.md         # 中文说明文档
├── README_EN.md      # 英文说明文档
└── LICENSE           # MIT 开源许可证
```

---

## 开源协议

本项目基于 [MIT License](LICENSE) 开源。
