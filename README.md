# Camera Simple Detect

Windows-only console service that detects whether a camera device is **real**, **virtual**, or **not detected**, and pushes updates to connected WebSocket clients.

## Features
- Enumerates camera devices via WMI on Windows.
- Heuristic detection for virtual camera software.
- WebSocket server for push notifications.
- Low memory footprint: no frame capture, only device enumeration.

## Requirements
- Windows 10/11
- .NET 6 SDK or runtime

## Configuration
Environment variables (optional):
- `CAMERA_WS_PORT` (default: `8787`)
- `CAMERA_DETECT_INTERVAL_MS` (default: `2000`)

## Run
```bash
dotnet run --project src/CameraSimpleDetect/CameraSimpleDetect.csproj
```

## WebSocket
Connect to:
```
ws://127.0.0.1:<PORT>/ws/
```

Message example:
```json
{"status":"real_camera","timestamp":"2024-01-01T00:00:00Z"}
```

## Status values
- `real_camera`
- `virtual_camera`
- `no_camera`
