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
