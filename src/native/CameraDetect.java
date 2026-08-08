// cameraSimpleDetect — zero-dependency Java implementation
// Camera detection (WMI/PowerShell) + WebSocket server (JDK built-in HttpServer)
// Compile: javac CameraDetect.java
// Run:     java CameraDetect [port] [interval_ms]

import java.io.*;
import java.net.*;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.time.Instant;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicBoolean;

// Note: We use raw ServerSocket (not com.sun.net.httpserver) for proper
// WebSocket upgrade — the JDK HttpServer consumes the TCP stream and
// cannot cleanly hand over the raw socket after 101 Switching Protocols.

// ══════════════════════════════════════════════════════════════════════════════
// CameraDetect — single-file, zero-external-JAR
// ══════════════════════════════════════════════════════════════════════════════
public class CameraDetect {

    // ── Constants ──────────────────────────────────────────────────────────
    private static final int    DEFAULT_PORT      = 8787;
    private static final int    DEFAULT_INTERVAL_MS = 2000;
    private static final String WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    private static final String[] VIRTUAL_KEYWORDS = {
        "virtual", "obs", "manycam",  "snap camera", "xsplit",
        "mmhmm",   "droidcam", "iriun", "contacam",  "streamlabs",
        "camsip",  "v4l2",    "epoccam", "vcam",     "ndi",
    };

    private static final String[][] VID_PID_BLACKLIST = {
        {"0bda", "58f4"}, // OBS Virtual Camera
        {"0c45", "6366"}, // ManyCam Virtual Webcam
        {"2b7e", "f13a"}, // Snap Camera
        {"05a3", "9331"}, // DroidCam
    };

    // ── Enums ──────────────────────────────────────────────────────────────
    enum CameraStatus {
        REAL_CAMERA("real_camera"),
        VIRTUAL_CAMERA("virtual_camera"),
        NO_CAMERA("no_camera"),
        UNKNOWN("unknown");

        final String json;
        CameraStatus(String j) { this.json = j; }
    }

    // ── State ──────────────────────────────────────────────────────────────
    private final int port;
    private final int intervalMs;
    private final AtomicBoolean running = new AtomicBoolean(true);
    private final Set<WsClient> clients = ConcurrentHashMap.newKeySet();
    private volatile CameraStatus lastStatus = CameraStatus.UNKNOWN;

    // ── Constructor ────────────────────────────────────────────────────────
    public CameraDetect(int port, int intervalMs) {
        this.port = port;
        this.intervalMs = Math.max(intervalMs, 500);
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Camera Detection (via PowerShell → WMI)
    // ═══════════════════════════════════════════════════════════════════════

    private List<String> enumerateDeviceNames() {
        List<String> names = new ArrayList<>();
        try {
            ProcessBuilder pb = new ProcessBuilder(
                "powershell.exe", "-NoProfile", "-Command",
                "Get-CimInstance Win32_PnPEntity -Filter \"PNPClass='Image'\" | " +
                "Select-Object -ExpandProperty Name"
            );
            pb.redirectErrorStream(true);
            Process proc = pb.start();
            try (BufferedReader reader = new BufferedReader(
                     new InputStreamReader(proc.getInputStream(), StandardCharsets.UTF_8))) {
                String line;
                while ((line = reader.readLine()) != null) {
                    line = line.trim();
                    if (!line.isEmpty() && !line.startsWith("Get-CimInstance")) {
                        names.add(line);
                    }
                }
            }
            proc.waitFor(5, TimeUnit.SECONDS);
        } catch (Exception e) {
            System.err.println("[WARN] PowerShell WMI query failed: " + e.getMessage());
        }
        return names;
    }

    private boolean isVirtual(String deviceName) {
        String lower = deviceName.toLowerCase(Locale.ROOT);
        for (String kw : VIRTUAL_KEYWORDS) {
            if (lower.contains(kw)) return true;
        }
        return false;
    }

    CameraStatus detectStatus() {
        List<String> deviceNames = enumerateDeviceNames();
        if (deviceNames.isEmpty()) return CameraStatus.NO_CAMERA;

        boolean hasVirtual = false, hasReal = false;
        for (String name : deviceNames) {
            if (isVirtual(name)) hasVirtual = true;
            else hasReal = true;
        }

        if (hasReal) return CameraStatus.REAL_CAMERA;
        if (hasVirtual) return CameraStatus.VIRTUAL_CAMERA;
        return CameraStatus.NO_CAMERA;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // SHA1 + Base64 (JDK built-in)
    // ═══════════════════════════════════════════════════════════════════════

    private static String sha1Base64(String input) {
        try {
            MessageDigest md = MessageDigest.getInstance("SHA-1");
            byte[] hash = md.digest(input.getBytes(StandardCharsets.UTF_8));
            return Base64.getEncoder().encodeToString(hash);
        } catch (NoSuchAlgorithmException e) {
            throw new RuntimeException("SHA-1 not available", e);
        }
    }

    static String computeWsAccept(String clientKey) {
        return sha1Base64(clientKey + WS_GUID);
    }

    // ═══════════════════════════════════════════════════════════════════════
    // JSON builder (no external lib)
    // ═══════════════════════════════════════════════════════════════════════

    static String buildStatusJson(CameraStatus status) {
        return String.format("{\"status\":\"%s\",\"timestamp\":\"%s\"}",
            status.json, Instant.now().toString());
    }

    // ═══════════════════════════════════════════════════════════════════════
    // WebSocket frame encoding (server → client, unmasked)
    // ═══════════════════════════════════════════════════════════════════════

    static byte[] encodeFrame(String payload) {
        byte[] data = payload.getBytes(StandardCharsets.UTF_8);
        int len = data.length;
        ByteArrayOutputStream bos = new ByteArrayOutputStream(len + 10);

        // FIN=1, opcode=1 (text)
        bos.write(0x81);

        // Payload length (server → client, no mask)
        if (len <= 125) {
            bos.write(len);
        } else if (len <= 65535) {
            bos.write(126);
            bos.write((len >> 8) & 0xFF);
            bos.write(len & 0xFF);
        } else {
            bos.write(127);
            for (int i = 7; i >= 0; i--)
                bos.write((int)((len >> (i * 8)) & 0xFF));
        }
        try {
            bos.write(data);
        } catch (IOException ignored) {}
        return bos.toByteArray();
    }

    // ═══════════════════════════════════════════════════════════════════════
    // WebSocket frame decoding (client → server, masked)
    // ═══════════════════════════════════════════════════════════════════════

    static String decodeFrame(byte[] data, int size) {
        if (size < 2) return null;
        int pos = 0;
        int opcode = data[pos] & 0x0F;
        boolean masked = (data[pos + 1] & 0x80) != 0;

        if (opcode == 0x8) return null; // close
        if (opcode == 0x9) return null; // ping

        pos += 2;
        long payloadLen = data[1] & 0x7F;
        if (payloadLen == 126) {
            if (size < pos + 2) return null;
            payloadLen = ((data[pos] & 0xFF) << 8) | (data[pos + 1] & 0xFF);
            pos += 2;
        } else if (payloadLen == 127) {
            if (size < pos + 8) return null;
            payloadLen = 0;
            for (int i = 0; i < 8; i++)
                payloadLen = (payloadLen << 8) | (data[pos + i] & 0xFF);
            pos += 8;
        }

        byte[] mask = new byte[4];
        if (masked) {
            if (size < pos + 4) return null;
            System.arraycopy(data, pos, mask, 0, 4);
            pos += 4;
        }

        if (size < pos + payloadLen) return null;

        byte[] payload = new byte[(int) payloadLen];
        for (int i = 0; i < payloadLen; i++) {
            payload[i] = masked ? (byte)(data[pos + i] ^ mask[i % 4]) : data[pos + i];
        }
        return new String(payload, StandardCharsets.UTF_8);
    }

    // ═══════════════════════════════════════════════════════════════════════
    // WebSocket Client Handler
    // ═══════════════════════════════════════════════════════════════════════

    class WsClient {
        final OutputStream out;
        final InputStream  in;
        final Socket       socket;
        volatile boolean   open = true;

        WsClient(Socket socket) throws IOException {
            this.socket = socket;
            this.out    = socket.getOutputStream();
            this.in     = socket.getInputStream();
        }

        void send(String message) {
            if (!open) return;
            try {
                synchronized (out) {
                    out.write(encodeFrame(message));
                    out.flush();
                }
            } catch (IOException e) {
                open = false;
            }
        }

        void close() {
            open = false;
            try { socket.close(); } catch (IOException ignored) {}
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Raw WebSocket Server (ServerSocket + manual HTTP upgrade)
    //
    // We use raw ServerSocket instead of com.sun.net.httpserver.HttpServer
    // because the latter cannot cleanly hand over the raw socket after
    // 101 Switching Protocols — it keeps consuming the TCP stream.
    // ═══════════════════════════════════════════════════════════════════════

    private void sendCurrentStatus(WsClient client) {
        String json = buildStatusJson(lastStatus);
        client.send(json);
    }

    private String getHttpHeader(String request, String key) {
        String search = key + ": ";
        int idx = request.indexOf(search);
        if (idx < 0) return "";
        idx += search.length();
        int end = request.indexOf("\r\n", idx);
        if (end < 0) return "";
        return request.substring(idx, end);
    }

    private boolean handleHttpUpgrade(Socket socket) throws IOException {
        InputStream in = socket.getInputStream();
        OutputStream out = socket.getOutputStream();

        // Read HTTP request
        ByteArrayOutputStream headerBuf = new ByteArrayOutputStream();
        byte[] buf = new byte[1];
        byte[] last4 = new byte[4];
        int idx = 0;

        socket.setSoTimeout(5000); // 5s handshake timeout
        while (true) {
            int n = in.read(buf, 0, 1);
            if (n <= 0) return false;
            headerBuf.write(buf[0]);
            last4[idx % 4] = buf[0];
            idx++;
            // Check for \r\n\r\n
            if (idx >= 4 &&
                last4[(idx-4)%4] == '\r' && last4[(idx-3)%4] == '\n' &&
                last4[(idx-2)%4] == '\r' && last4[(idx-1)%4] == '\n') {
                break;
            }
            if (idx > 8192) return false; // too large
        }
        socket.setSoTimeout(0); // back to blocking

        String request = headerBuf.toString(StandardCharsets.UTF_8);

        // Check for WebSocket upgrade
        if (!request.contains("Upgrade: websocket") || !request.contains("GET /ws")) {
            String resp = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n" +
                          "Content-Length: 45\r\n\r\n" +
                          "cameraSimpleDetect WebSocket at /ws\n";
            out.write(resp.getBytes(StandardCharsets.UTF_8));
            out.flush();
            return false;
        }

        String wsKey = getHttpHeader(request, "Sec-WebSocket-Key");
        if (wsKey.isEmpty()) {
            String resp = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
            out.write(resp.getBytes(StandardCharsets.UTF_8));
            out.flush();
            return false;
        }

        String acceptKey = computeWsAccept(wsKey);
        String response = "HTTP/1.1 101 Switching Protocols\r\n" +
                          "Upgrade: websocket\r\n" +
                          "Connection: Upgrade\r\n" +
                          "Sec-WebSocket-Accept: " + acceptKey + "\r\n" +
                          "\r\n";
        out.write(response.getBytes(StandardCharsets.UTF_8));
        out.flush();
        return true;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Server Start
    // ═══════════════════════════════════════════════════════════════════════

    void start() throws IOException {
        @SuppressWarnings("resource")
        ServerSocket serverSocket = new ServerSocket(port, 50,
            InetAddress.getByName("127.0.0.1"));
        System.out.println("WebSocket server listening on ws://127.0.0.1:" + port + "/ws");
        System.out.println("Connect to ws://127.0.0.1:" + port + "/ws to receive camera status");
        System.out.println("Press Ctrl+C to stop\n");

        // Start monitor thread
        ScheduledExecutorService scheduler = Executors.newSingleThreadScheduledExecutor();
        scheduler.scheduleWithFixedDelay(this::monitorLoop, 0,
            intervalMs, TimeUnit.MILLISECONDS);

        // Accept loop
        while (running.get()) {
            try {
                Socket clientSocket = serverSocket.accept();
                new Thread(() -> handleClient(clientSocket), "client").start();
            } catch (IOException e) {
                if (running.get()) {
                    System.err.println("[Server] Accept error: " + e.getMessage());
                }
            }
        }

        scheduler.shutdown();
        try { serverSocket.close(); } catch (IOException ignored) {}
    }

    private void handleClient(Socket socket) {
        try {
            if (!handleHttpUpgrade(socket)) {
                try { socket.close(); } catch (IOException ignored) {}
                return;
            }

            WsClient client = new WsClient(socket);
            clients.add(client);
            sendCurrentStatus(client);

            byte[] buf = new byte[4096];
            InputStream in = socket.getInputStream();

            while (running.get() && client.open) {
                int n;
                try {
                    n = in.read(buf);
                } catch (SocketTimeoutException e) {
                    continue;
                }
                if (n <= 0) break;

                String msg = decodeFrame(buf, n);
                if (msg == null) break; // close frame
                // Echo or ignore
            }
        } catch (IOException e) {
            // client disconnected
        } finally {
            // Cleanup happens via open flag + clients set
        }
    }

    private void monitorLoop() {
        if (!running.get()) return;
        CameraStatus current = detectStatus();

        if (current != lastStatus) {
            lastStatus = current;
            String json = buildStatusJson(current);
            System.out.println("[Detect] Status changed: " + current.json);

            // Broadcast to all clients
            for (WsClient client : clients) {
                client.send(json);
            }
        }
    }

    void shutdown() {
        running.set(false);
        for (WsClient client : clients) {
            client.close();
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Main
    // ═══════════════════════════════════════════════════════════════════════

    public static void main(String[] args) throws Exception {
        int port     = DEFAULT_PORT;
        int interval = DEFAULT_INTERVAL_MS;

        // Parse args
        for (int i = 0; i < args.length; i++) {
            switch (args[i]) {
                case "--help":
                case "-h":
                    System.out.println("cameraSimpleDetect v0.2.0 (Java — zero dependencies)");
                    System.out.println("Usage: java CameraDetect [--port PORT] [--interval MS]");
                    System.out.println("  --port     PORT   Server port (default: " + DEFAULT_PORT + ")");
                    System.out.println("  --interval MS     Detection interval in ms (default: " + DEFAULT_INTERVAL_MS + ")");
                    System.out.println("  --help            Show this help");
                    return;
                case "--port":
                case "-p":
                    if (i + 1 < args.length) port = Integer.parseInt(args[++i]);
                    break;
                case "--interval":
                case "-i":
                    if (i + 1 < args.length) interval = Integer.parseInt(args[++i]);
                    break;
                default:
                    // Positional: first number = port, second = interval
                    try {
                        int v = Integer.parseInt(args[i]);
                        if (i == 0 || args[i-1].startsWith("-")) {
                            // After a flag, skip (handled above)
                        }
                    } catch (NumberFormatException ignored) {}
                    break;
            }
        }

        // Also check positional args
        int posNums = 0;
        for (String arg : args) {
            if (arg.startsWith("-")) continue;
            try {
                int v = Integer.parseInt(arg);
                if (posNums == 0 && v > 0 && v <= 65535) port = v;
                else if (posNums == 1 && v >= 500) interval = v;
                posNums++;
            } catch (NumberFormatException ignored) {}
        }

        System.out.println("cameraSimpleDetect v0.2.0 (Java)");
        System.out.println("Port: " + port + ", Interval: " + interval + "ms");

        CameraDetect app = new CameraDetect(port, interval);

        Runtime.getRuntime().addShutdownHook(new Thread(app::shutdown));
        app.start();
    }
}
