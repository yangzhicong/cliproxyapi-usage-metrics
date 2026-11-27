# cliproxyapi-usage-metrics

[中文](README.md) | [English](README_EN.md)

A lightweight, zero-dependency C ABI (`usage_plugin`) observability plugin for [CLIProxyAPI](https://github.com/router-for-me/CLIProxyAPI) with an embedded real-time Web dashboard.

---

## Features

- **Zero External Dependencies (Pure C + POSIX)**: Compiles to a single ~83 KB `.so` shared library using standard C/POSIX libraries (`-pthread`), embedding the entire dashboard HTML at build time (`gen_ui_header.py` -> `ui_assets.h`).
- **Non-Intrusive Hot Path**: Subscribes asynchronously via CLIProxyAPI's `usage_plugin` C ABI hooks without blocking or modifying upstream LLM forwarding.
- **Comprehensive Token & Cache Accounting**:
  - Tracks total input tokens, uncached (miss) input tokens, cached read tokens, output tokens, and reasoning/thinking tokens.
  - Calculates overall and per-request **Prompt Cache Hit Rate** (`cache_read_tokens / input_tokens`) and cache-hit request counts.
- **Latency & Decode Speed Metrics**:
  - Measures **TTFT (Time to First Token)** — average, min, and max — and end-to-end latency.
  - Computes real decode throughput (**token/s**) separately for streaming vs. non-streaming requests (`(output + reasoning) / decode_duration`).
- **Multi-Dimensional Breakdown & Filtering**:
  - Time range selector (`Today` / `Last 7 Days` / `All`).
  - Aggregated tables **by Model** and **by Account (`auth_id`)**.
  - Paginated recent request log with **Model** and **Account** filters, plus one-click JSON export and reset.
- **Crash-Safe Atomic Persistence**: Periodically flushes state (`plugins/usage-metrics.state.json`) using atomic write-and-rename (every 5 seconds or every 10 records, plus clean flush on shutdown) while maintaining a 4,000-record ring buffer and lifetime totals.

---

## Build

Requires `gcc` (Linux) and `python3` (for embedding `ui.html`).

```bash
git clone https://github.com/yangzhicong/cliproxyapi-usage-metrics.git
cd cliproxyapi-usage-metrics

# Option 1: build script
./build.sh ./usage-metrics.so

# Option 2: Make
make
```

The build runs with `-O2 -Wall -Wextra -Werror -pthread` and produces `usage-metrics.so`.

---

## Deployment & Usage

1. Copy the compiled shared library into your CLIProxyAPI plugin directory:

```bash
cp usage-metrics.so /path/to/CLIProxyAPI/plugins/linux/amd64/usage-metrics.so
chmod 755 /path/to/CLIProxyAPI/plugins/linux/amd64/usage-metrics.so
```

2. Restart CLIProxyAPI (or trigger plugin reload) and confirm registration in logs:

```text
pluginhost: plugin registered plugin_id=usage-metrics plugin_name=usage-metrics version=1.0.10
```

3. Open the embedded dashboard in your browser or query the JSON API:

- **Web UI**: `http://<your-cpa-host>/v0/resource/plugins/usage-metrics/ui`
- **JSON API**:
  - Summary: `GET /v0/resource/plugins/usage-metrics/api?op=summary&range=today&limit=500` (supports optional `&model=...&auth=...`)
  - Export full state: `GET /v0/resource/plugins/usage-metrics/api?op=export`
  - Clear statistics: `GET /v0/resource/plugins/usage-metrics/api?op=clear`

---

## Project Structure

```text
.
├── plugin.c          # C ABI v1 plugin implementation, JSON parser/builder, ring buffer & persistence
├── ui.html           # Single-file responsive Web dashboard (auto-refresh, pagination, filters)
├── gen_ui_header.py  # Encodes ui.html into base64 C header (ui_assets.h) at build time
├── build.sh          # One-command build script
├── Makefile          # Standard Make build target
├── README.md         # Chinese documentation
├── README_EN.md      # English documentation
└── LICENSE           # MIT License
```

---

## License

Licensed under the [MIT License](LICENSE).
