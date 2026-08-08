mod websocket;

use camera_simple_detect::camera::{detect_cameras, DetectionResult};
use std::sync::Mutex;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Duration;

const DEFAULT_PORT: u16 = 8787;
const DEFAULT_INTERVAL_MS: u64 = 2000;

fn status_to_json(status: DetectionResult) -> String {
    let status_str = match status {
        DetectionResult::RealCamera => "real_camera",
        DetectionResult::VirtualCamera => "virtual_camera",
        DetectionResult::NoCamera => "no_camera",
    };

    // ISO-8601 UTC timestamp
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default();
    let secs = now.as_secs();
    let days = secs / 86400;

    // Simple date calculation (works until year 2100)
    let mut y = 1970i32;
    let mut remaining = days as i32;
    loop {
        let year_days = if is_leap(y) { 366 } else { 365 };
        if remaining < year_days { break; }
        remaining -= year_days;
        y += 1;
    }
    let month_days = if is_leap(y) {
        [31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
    } else {
        [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
    };
    let mut m = 0usize;
    while m < 12 && remaining >= month_days[m] {
        remaining -= month_days[m];
        m += 1;
    }
    let month = m + 1;
    let day = remaining + 1;

    let time_secs = secs % 86400;
    let hour = time_secs / 3600;
    let minute = (time_secs % 3600) / 60;
    let second = time_secs % 60;

    format!(
        r#"{{"status":"{}","timestamp":"{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z"}}"#,
        status_str, y, month, day, hour, minute, second
    )
}

fn is_leap(year: i32) -> bool {
    (year % 4 == 0 && year % 100 != 0) || year % 400 == 0
}

fn print_usage() {
    println!("cameraSimpleDetect v0.2.0 (Rust)");
    println!("Usage: camera_simple_detect.exe [--port PORT] [--interval MS]");
    println!("  --port     PORT   Server port (default: {})", DEFAULT_PORT);
    println!("  --interval MS     Detection interval in ms (default: {})", DEFAULT_INTERVAL_MS);
    println!("  --help            Show this help");
}

fn main() {
    let mut port = DEFAULT_PORT;
    let mut interval_ms = DEFAULT_INTERVAL_MS;

    // Parse CLI args
    let args: Vec<String> = std::env::args().collect();
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--help" | "-h" => {
                print_usage();
                return;
            }
            "--port" | "-p" => {
                if i + 1 < args.len() {
                    if let Ok(p) = args[i + 1].parse() {
                        port = p;
                    }
                    i += 1;
                }
            }
            "--interval" | "-i" => {
                if i + 1 < args.len() {
                    if let Ok(iv) = args[i + 1].parse() {
                        interval_ms = iv.max(500);
                    }
                    i += 1;
                }
            }
            _ => {}
        }
        i += 1;
    }

    println!("cameraSimpleDetect v0.2.0 (Rust)");
    println!("Port: {}, Interval: {}ms", port, interval_ms);

    let clients = Arc::new(Mutex::new(Vec::new()));
    let server = websocket::WsServer::with_clients(Arc::clone(&clients));
    let running = Arc::new(AtomicBool::new(true));
    let running_clone = Arc::clone(&running);

    // Monitor thread — polls camera status and broadcasts changes
    let broadcast_clients = Arc::clone(&clients);

    thread::spawn(move || {
        println!("[Monitor] Camera detection started");
        let mut last_status: Option<DetectionResult> = None;

        while running_clone.load(Ordering::Relaxed) {
            let current = detect_cameras();
            let changed = match last_status {
                Some(ref s) if *s == current => false,
                _ => true,
            };

            if changed {
                let status_str = match current {
                    DetectionResult::RealCamera => "real_camera",
                    DetectionResult::VirtualCamera => "virtual_camera",
                    DetectionResult::NoCamera => "no_camera",
                };
                println!("[Detect] Status changed: {}", status_str);
                let json = status_to_json(current);
                websocket::broadcast(&broadcast_clients, &json);
                last_status = Some(current);
            }

            // Sleep in small chunks for responsive shutdown
            let mut slept = 0u64;
            while slept < interval_ms && running_clone.load(Ordering::Relaxed) {
                thread::sleep(Duration::from_millis(100.min(interval_ms - slept)));
                slept += 100;
            }
        }
        println!("[Monitor] Stopped");
    });

    // Start WebSocket server (blocks on accept loop)
    if let Err(e) = server.start(port) {
        eprintln!("[Server] Error: {}", e);
    }

    running.store(false, Ordering::Relaxed);
    println!("Server stopped.");
}
