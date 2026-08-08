# cameraSimpleDetect

一个用于 Windows 摄像头检测与示例的项目骨架。

## 技术选型

- 语言与生态：**Rust + windows-rs**
- 目标平台：**仅适配 Windows**（Windows 10/11）

后续计划通过 `windows` crate 调用 Windows API（如 Media Foundation 或相关设备枚举接口）实现摄像头检测与能力查询。

## 目录结构

```
.
├── cmd/        # CLI 命令与扩展入口
├── configs/    # 配置文件模板
├── docs/       # 设计与使用文档
├── src/        # 主程序源码
└── Cargo.toml  # Rust 依赖与构建配置
```

## 构建与运行

```bash
cargo run
```

程序启动后会输出版本信息。
用于摄像头检测与事件推送的简化服务。

## 系统要求

- Windows 10/11（64 位）
- Microsoft Visual C++ 2015-2022 Redistributable (x64)

## 快速启动

1. 复制配置模板并按需修改：

   ```powershell
   copy .\configs\config.template.json .\configs\config.json
   notepad .\configs\config.json
   ```

2. 启动服务（示例）：

   ```powershell
   .\cameraSimpleDetect.exe --config .\configs\config.json
   ```

3. 默认监听 `http://0.0.0.0:9000`，WebSocket 连接地址为 `ws://localhost:9000/ws`。

## 配置说明

配置模板位于 `configs/config.template.json`，包含端口、检测频率与黑名单规则：

- `server.port`: 服务监听端口。
- `detection.interval_ms`: 检测频率（毫秒）。
- `blacklist.rules`: 黑名单规则数组，支持 `ip`、`camera_id`、`regex`。

## WebSocket 协议

### 消息格式

所有消息采用 JSON：

```json
{
  "type": "event",
  "timestamp": "2024-01-01T12:00:00Z",
  "data": {}
}
```

字段说明：

- `type`: 消息类型，常见为 `event`、`heartbeat`。
- `timestamp`: ISO-8601 时间戳。
- `data`: 业务数据负载。

### 示例

**检测事件**

```json
{
  "type": "event",
  "timestamp": "2024-01-01T12:00:00Z",
  "data": {
    "camera_id": "CAM-001",
    "label": "person",
    "score": 0.96
  }
}
```

**心跳**

```json
{
  "type": "heartbeat",
  "timestamp": "2024-01-01T12:00:05Z",
  "data": {
    "status": "ok"
  }
}
```
