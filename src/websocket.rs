// WebSocket server — zero external crate, std::net::TcpListener only.
// Hand-rolled SHA1 + Base64 for the RFC 6455 opening handshake.

use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::{Arc, Mutex};
use std::thread;

const WS_GUID: &str = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// ═══════════════════════════════════════════════════════════════════════════
// SHA1 (hand-rolled, zero-dependency)
// ═══════════════════════════════════════════════════════════════════════════

fn sha1(input: &[u8]) -> [u8; 20] {
    // Padding: append 0x80, then zero-pad until (len + 8) % 64 == 0, then 64-bit length
    let ml_bits = (input.len() as u64) * 8;

    let mut msg = Vec::with_capacity(input.len() + 1 + 64 + 8);
    msg.extend_from_slice(input);
    msg.push(0x80u8);
    while (msg.len() + 8) % 64 != 0 {
        msg.push(0x00u8);
    }
    msg.extend_from_slice(&ml_bits.to_be_bytes());

    // Initialize state
    let mut h: [u32; 5] = [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0];

    // Process 64-byte blocks
    for chunk in msg.chunks(64) {
        let mut w = [0u32; 80];
        for i in 0..16 {
            w[i] = u32::from_be_bytes([
                chunk[i*4], chunk[i*4+1], chunk[i*4+2], chunk[i*4+3],
            ]);
        }
        for i in 16..80 {
            w[i] = (w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16]).rotate_left(1);
        }

        let [mut a, mut b, mut c, mut d, mut e] = h;

        for i in 0..80 {
            let (f, k): (u32, u32) = match i {
                0..=19  => ((b & c) | (!b & d), 0x5A827999),
                20..=39 => (b ^ c ^ d,          0x6ED9EBA1),
                40..=59 => ((b & c) | (b & d) | (c & d), 0x8F1BBCDC),
                _       => (b ^ c ^ d,          0xCA62C1D6),
            };
            let temp = a.rotate_left(5).wrapping_add(f).wrapping_add(e).wrapping_add(k).wrapping_add(w[i]);
            e = d;
            d = c;
            c = b.rotate_left(30);
            b = a;
            a = temp;
        }

        h[0] = h[0].wrapping_add(a);
        h[1] = h[1].wrapping_add(b);
        h[2] = h[2].wrapping_add(c);
        h[3] = h[3].wrapping_add(d);
        h[4] = h[4].wrapping_add(e);
    }

    let mut result = [0u8; 20];
    for i in 0..5 {
        result[i*4..i*4+4].copy_from_slice(&h[i].to_be_bytes());
    }
    result
}

// ═══════════════════════════════════════════════════════════════════════════
// Base64 encode (hand-rolled, zero-dependency)
// ═══════════════════════════════════════════════════════════════════════════

fn base64_encode(data: &[u8]) -> String {
    const TABLE: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = Vec::with_capacity((data.len() + 2) / 3 * 4);

    for chunk in data.chunks(3) {
        match chunk.len() {
            3 => {
                let n = (chunk[0] as u32) << 16 | (chunk[1] as u32) << 8 | chunk[2] as u32;
                out.push(TABLE[((n >> 18) & 0x3F) as usize]);
                out.push(TABLE[((n >> 12) & 0x3F) as usize]);
                out.push(TABLE[((n >> 6)  & 0x3F) as usize]);
                out.push(TABLE[( n        & 0x3F) as usize]);
            }
            2 => {
                let n = (chunk[0] as u32) << 16 | (chunk[1] as u32) << 8;
                out.push(TABLE[((n >> 18) & 0x3F) as usize]);
                out.push(TABLE[((n >> 12) & 0x3F) as usize]);
                out.push(TABLE[((n >> 6)  & 0x3F) as usize]);
                out.push(b'=');
            }
            _ => {
                let n = (chunk[0] as u32) << 16;
                out.push(TABLE[((n >> 18) & 0x3F) as usize]);
                out.push(TABLE[((n >> 12) & 0x3F) as usize]);
                out.push(b'=');
                out.push(b'=');
            }
        }
    }
    String::from_utf8(out).unwrap()
}

pub fn ws_accept_key(client_key: &str) -> String {
    base64_encode(&sha1(format!("{}{}", client_key, WS_GUID).as_bytes()))
}

// ═══════════════════════════════════════════════════════════════════════════
// WebSocket frame encode (server → client, unmasked)
// ═══════════════════════════════════════════════════════════════════════════

pub fn ws_encode_frame(payload: &[u8]) -> Vec<u8> {
    let len = payload.len();
    let mut frame = Vec::with_capacity(len + 10);

    // FIN=1, opcode=1 (text)
    frame.push(0x81u8);

    if len <= 125 {
        frame.push(len as u8);
    } else if len <= 65535 {
        frame.push(126u8);
        frame.extend_from_slice(&(len as u16).to_be_bytes());
    } else {
        frame.push(127u8);
        frame.extend_from_slice(&(len as u64).to_be_bytes());
    }

    frame.extend_from_slice(payload);
    frame
}

/// Build a text WebSocket frame from a string
pub fn ws_encode_text(text: &str) -> Vec<u8> {
    ws_encode_frame(text.as_bytes())
}

// ═══════════════════════════════════════════════════════════════════════════
// WebSocket frame decode (client → server, masked)
// Returns (opcode, payload_bytes) or None on close/error.
// ═══════════════════════════════════════════════════════════════════════════

#[derive(Debug, PartialEq)]
pub enum WsOpcode {
    Text,
    Close,
    Ping,
    Pong,
}

pub fn ws_decode_frame(data: &[u8]) -> Option<(WsOpcode, Vec<u8>)> {
    if data.len() < 2 { return None; }

    let opcode = data[0] & 0x0F;
    let masked = (data[1] & 0x80) != 0;
    let op = match opcode {
        0x01 => WsOpcode::Text,
        0x08 => WsOpcode::Close,
        0x09 => WsOpcode::Ping,
        0x0A => WsOpcode::Pong,
        _    => return None,
    };

    let mut pos = 2usize;
    let mut payload_len = (data[1] & 0x7F) as usize;

    if payload_len == 126 {
        if data.len() < pos + 2 { return None; }
        payload_len = u16::from_be_bytes([data[pos], data[pos+1]]) as usize;
        pos += 2;
    } else if payload_len == 127 {
        if data.len() < pos + 8 { return None; }
        payload_len = u64::from_be_bytes([
            data[pos], data[pos+1], data[pos+2], data[pos+3],
            data[pos+4], data[pos+5], data[pos+6], data[pos+7],
        ]) as usize;
        pos += 8;
    }

    let mut mask = [0u8; 4];
    if masked {
        if data.len() < pos + 4 { return None; }
        mask.copy_from_slice(&data[pos..pos+4]);
        pos += 4;
    }

    if data.len() < pos + payload_len { return None; }

    let mut payload = vec![0u8; payload_len];
    for i in 0..payload_len {
        payload[i] = if masked { data[pos+i] ^ mask[i%4] } else { data[pos+i] };
    }

    Some((op, payload))
}

// ═══════════════════════════════════════════════════════════════════════════
// HTTP upgrade handshake
// ═══════════════════════════════════════════════════════════════════════════

fn get_header(request: &str, key: &str) -> Option<String> {
    let search = format!("{}: ", key);
    request.find(&search).and_then(|pos| {
        let start = pos + search.len();
        request[start..].find("\r\n").map(|end| {
            request[start..start+end].trim().to_string()
        })
    })
}

pub fn try_handshake(stream: &mut TcpStream) -> bool {
    // Read HTTP upgrade request
    let mut buf = [0u8; 1];
    let mut request = Vec::new();
    stream.set_read_timeout(Some(std::time::Duration::from_secs(5))).ok();

    loop {
        match stream.read(&mut buf) {
            Ok(1) => {
                request.push(buf[0]);
                let len = request.len();
                if len >= 4
                    && request[len-4] == b'\r' && request[len-3] == b'\n'
                    && request[len-2] == b'\r' && request[len-1] == b'\n'
                {
                    break;
                }
                if len > 8192 { return false; }
            }
            _ => return false,
        }
    }
    stream.set_read_timeout(None).ok();

    let req_str = String::from_utf8_lossy(&request);

    // Validate: GET /ws and Upgrade: websocket
    if !req_str.contains("GET /ws") || !req_str.contains("Upgrade: websocket") {
        // Return plain-text info for non-WebSocket requests
        let body = "cameraSimpleDetect WebSocket server. Connect to /ws\n";
        let resp = format!(
            "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: {}\r\n\r\n{}",
            body.len(), body
        );
        let _ = stream.write_all(resp.as_bytes());
        return false;
    }

    let ws_key = match get_header(&req_str, "Sec-WebSocket-Key") {
        Some(k) => k,
        None => {
            let _ = stream.write_all(b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n");
            return false;
        }
    };

    let accept = ws_accept_key(&ws_key);
    let response = format!(
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {}\r\n\r\n",
        accept
    );
    stream.write_all(response.as_bytes()).is_ok()
}

// ═══════════════════════════════════════════════════════════════════════════
// WebSocket Server
// ═══════════════════════════════════════════════════════════════════════════

pub struct WsServer {
    clients: Arc<Mutex<Vec<TcpStream>>>,
}

impl WsServer {
    pub fn new() -> Self {
        WsServer { clients: Arc::new(Mutex::new(Vec::new())) }
    }

    /// Create a server that shares the given client list (for broadcasting from another thread)
    pub fn with_clients(clients: Arc<Mutex<Vec<TcpStream>>>) -> Self {
        WsServer { clients }
    }

    /// Start the WebSocket server on the given port. Blocks until error.
    /// Spawns a thread per client to read frames.
    pub fn start(&self, port: u16) -> std::io::Result<()> {
        let listener = TcpListener::bind(("127.0.0.1", port))?;
        println!("WebSocket server listening on ws://127.0.0.1:{}/ws", port);
        println!("Connect to ws://127.0.0.1:{}/ws to receive camera status", port);
        println!("Press Ctrl+C to stop\n");

        for incoming in listener.incoming() {
            match incoming {
                Ok(mut stream) => {
                    if !try_handshake(&mut stream) {
                        // Handshake failed — close (already sent error)
                        let _ = stream.shutdown(std::net::Shutdown::Both);
                        continue;
                    }

                    // Handshake OK — register client
                    {
                        let mut clients = self.clients.lock().unwrap();
                        clients.push(stream.try_clone().unwrap());
                    }

                    let clients = Arc::clone(&self.clients);
                    thread::spawn(move || {
                        handle_client(stream, clients);
                    });
                }
                Err(e) => {
                    eprintln!("[Server] Accept error: {}", e);
                }
            }
        }
        Ok(())
    }

    /// Broadcast a text message to all connected clients.
    /// Disconnects broken clients.
    pub fn broadcast(&self, text: &str) {
        broadcast(&self.clients, text);
    }
}

/// Broadcast a text message to all clients in the shared list.
/// Removes disconnected clients automatically.
pub fn broadcast(clients: &Arc<Mutex<Vec<TcpStream>>>, text: &str) {
    let frame = ws_encode_text(text);
    if let Ok(mut list) = clients.lock() {
        list.retain_mut(|stream| {
            stream.write_all(&frame).is_ok()
        });
    }
}

/// Client read loop: reads frames until close or disconnect.
fn handle_client(mut stream: TcpStream, clients: Arc<Mutex<Vec<TcpStream>>>) {
    let mut buf = [0u8; 4096];
    loop {
        match stream.read(&mut buf) {
            Ok(0) => break, // EOF
            Ok(n) => {
                if let Some((op, _payload)) = ws_decode_frame(&buf[..n]) {
                    match op {
                        WsOpcode::Close => break,
                        WsOpcode::Ping => {
                            // Respond with pong
                            let pong = vec![0x8Au8, 0x00];
                            let _ = stream.write_all(&pong);
                        }
                        WsOpcode::Pong => {} // Ignore
                        WsOpcode::Text => {
                            // Echo received text (debug)
                            let frame = ws_encode_frame(&_payload);
                            let _ = stream.write_all(&frame);
                        }
                    }
                }
            }
            Err(_) => break,
        }
    }

    let _ = stream.shutdown(std::net::Shutdown::Both);
    // Remove from client list
    if let Ok(mut clients) = clients.lock() {
        clients.retain(|c| c.peer_addr().ok() != stream.peer_addr().ok());
    }
}
