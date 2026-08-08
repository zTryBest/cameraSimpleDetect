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

## 目录结构

```
.
├── cmd/                    # CLI 命令与扩展入口
├── configs/                # 配置文件模板
├── docs/                   # 设计与使用文档
├── src/
│   ├── main.rs             # Rust 版本入口
│   ├── camera/device_enum.rs  # Rust 摄像头检测核心
│   ├── CameraSimpleDetect/ # C# 版本（WebSocket 服务）
│   ├── native/             # ★ 零依赖版本
│   │   ├── camera_detect.cpp   # C++ 单文件实现
│   │   ├── CameraDetect.java   # Java 单文件实现
│   │   └── build.bat           # 一键编译脚本
│   ├── device_monitor.py   # Python 设备监控
│   └── network/            # C++ WebSocket（Boost 依赖）
└── Cargo.toml              # Rust 依赖与构建配置
```

## 系统要求

- Windows 10/11（64 位）

## 零依赖运行（推荐）

**不需要安装任何 SDK/运行时！** 提供了两个零外部依赖的实现：

### C++ 原生版本（推荐）

编译产物为单个 `.exe`，可拷贝到任何 Windows 10/11 电脑直接运行：

```powershell
# 编译（需要 MSVC Build Tools，一次性）
cd src\native
build.bat cpp
# 或手动编译：
# cl /O2 /EHsc /MT /std:c++17 camera_detect.cpp /Fe:camera_detect.exe

# 运行
.\camera_detect.exe                          # 默认 ws://127.0.0.1:8787/ws
.\camera_detect.exe --port 9000              # 自定义端口
.\camera_detect.exe --port 8787 --interval 2000  # 自定义端口和检测间隔
```

### Java 版本

适合已有 JDK 的开发者，无需任何 JAR 包：

```powershell
# 编译
cd src\native
build.bat java
# 或手动：javac CameraDetect.java

# 运行
java CameraDetect                  # 默认 ws://127.0.0.1:8787/ws
java CameraDetect 9000 2000        # 自定义端口和间隔
```

### 连接测试

```javascript
// 浏览器控制台
ws = new WebSocket('ws://127.0.0.1:8787/ws');
ws.onmessage = e => console.log(JSON.parse(e.data));
// → {"status":"real_camera","timestamp":"2024-01-01T12:00:00Z"}
```

```powershell
# PowerShell（无需任何安装）
$ws = [System.Net.WebSockets.ClientWebSocket]::new()
$ws.ConnectAsync('ws://127.0.0.1:8787/ws', [System.Threading.CancellationToken]::None).Wait()
```

## 旧版运行方式（需依赖）

### Rust 版本

```powershell
cargo run
```

### C# 版本

```bash
cd src/CameraSimpleDetect
dotnet run
```

### 配置文件

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
