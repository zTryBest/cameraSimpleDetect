// cameraSimpleDetect — zero-dependency C++ implementation
// Camera detection (Media Foundation + DirectShow) + WebSocket server (Winsock2)
// Build: cl /EHsc /std:c++17 /O2 /Fe:cameraSimpleDetect.exe camera_detect.cpp
//   mf.lib mfplat.lib strmiids.lib ws2_32.lib ole32.lib advapi32.lib crypt32.lib

#ifndef _WIN32
#error This program only runs on Windows.
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#endif

// winsock2.h MUST come before windows.h
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <dshow.h>
#include <wincrypt.h>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "crypt32.lib")

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ══════════════════════════════════════════════════════════════════════════════
// Constants
// ══════════════════════════════════════════════════════════════════════════════
constexpr int    DEFAULT_PORT      = 8787;
constexpr int    DEFAULT_INTERVAL_MS = 2000;
constexpr int    MAX_CLIENTS       = 32;
constexpr int    RECV_BUFFER_SIZE  = 4096;
constexpr size_t SHA1_HASH_LEN     = 20;

// WebSocket magic GUID per RFC 6455
constexpr const char* WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// Virtual camera keyword blacklist (same logic as src/camera/device_enum.rs)
constexpr const char* VIRTUAL_KEYWORDS[] = {
    "virtual", "obs", "manycam",  "snap camera", "xsplit",
    "mmhmm",   "droidcam", "iriun", "contacam",  "streamlabs",
    "camsip",  "v4l2",    "epoccam", "vcam",     "ndi",
};

// VID/PID blacklist for known virtual cameras
struct VidPidPair { const char* vid; const char* pid; };
constexpr VidPidPair VID_PID_BLACKLIST[] = {
    {"0bda", "58f4"}, // OBS Virtual Camera
    {"0c45", "6366"}, // ManyCam Virtual Webcam
    {"2b7e", "f13a"}, // Snap Camera
    {"05a3", "9331"}, // DroidCam
};

// CLSID blacklist for virtual camera filters
constexpr const char* CLSID_BLACKLIST[] = {
    "{860bb310-5d01-11d0-bd3b-00a0c911ce86}", // VideoInputDeviceCategory
    "{e5323777-f976-4f5b-9b55-b94699c46e44}", // SampleGrabber
};

// ══════════════════════════════════════════════════════════════════════════════
// Utility: Case-insensitive string operations
// ══════════════════════════════════════════════════════════════════════════════
static std::string to_lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

static bool contains_any(const std::string& haystack,
                         const char* const* needles, size_t count) {
    std::string lower = to_lower(haystack);
    for (size_t i = 0; i < count; ++i) {
        if (lower.find(needles[i]) != std::string::npos) return true;
    }
    return false;
}

// ══════════════════════════════════════════════════════════════════════════════
// Camera Device Info
// ══════════════════════════════════════════════════════════════════════════════
enum class CameraStatus { RealCamera, VirtualCamera, NoCamera, Unknown };

struct CameraDevice {
    std::string name;
    std::string manufacturer;
    std::string device_path;
    std::string driver;
    std::string vid;
    std::string pid;
    std::string clsid;
};

// Extract VID/PID from device path (e.g. "...vid_0bda&pid_58f4...")
static void parse_vid_pid(const std::string& path, std::string& vid,
                          std::string& pid) {
    auto extract = [&](const char* token) -> std::string {
        std::string lower = to_lower(path);
        size_t pos = lower.find(token);
        if (pos == std::string::npos) return "";
        pos += strlen(token);
        if (pos + 4 > lower.size()) return "";
        return lower.substr(pos, 4);
    };
    vid = extract("vid_");
    pid = extract("pid_");
}

// ══════════════════════════════════════════════════════════════════════════════
// Virtual Camera Detection
// ══════════════════════════════════════════════════════════════════════════════
static bool is_virtual_camera(const CameraDevice& dev) {
    // Build searchable haystack
    std::string haystack = dev.name + dev.manufacturer + dev.driver + dev.device_path;

    // Keyword blacklist
    if (contains_any(haystack, VIRTUAL_KEYWORDS,
                     sizeof(VIRTUAL_KEYWORDS) / sizeof(VIRTUAL_KEYWORDS[0])))
        return true;

    // CLSID blacklist
    for (const auto& cls : CLSID_BLACKLIST) {
        if (to_lower(dev.clsid).find(cls + 1) != std::string::npos) return true;
    }

    // VID/PID blacklist
    if (!dev.vid.empty() && !dev.pid.empty()) {
        for (const auto& vp : VID_PID_BLACKLIST) {
            if (dev.vid == vp.vid && dev.pid == vp.pid) return true;
        }
    }

    return false;
}

// ══════════════════════════════════════════════════════════════════════════════
// Camera Enumeration — Media Foundation
// ══════════════════════════════════════════════════════════════════════════════
static std::vector<CameraDevice> enumerate_mf_devices() {
    std::vector<CameraDevice> devices;

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return devices;
    if (FAILED(MFStartup(MF_VERSION, 0))) {
        CoUninitialize();
        return devices;
    }

    IMFAttributes* attrs = nullptr;
    if (FAILED(MFCreateAttributes(&attrs, 1))) goto mf_cleanup;

    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                   MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    {
        IMFActivate** activates = nullptr;
        UINT32 count = 0;
        if (SUCCEEDED(MFEnumDeviceSources(attrs, &activates, &count)) && activates) {
            for (UINT32 i = 0; i < count; ++i) {
                IMFActivate* act = activates[i];
                if (!act) continue;

                CameraDevice dev;
                WCHAR* wstr = nullptr;
                UINT32 len = 0;

                // Friendly name
                if (SUCCEEDED(act->GetAllocatedString(
                        &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &wstr, &len)) &&
                    wstr) {
                    char buf[512] = {};
                    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, buf,
                                        sizeof(buf) - 1, nullptr, nullptr);
                    dev.name = buf;
                    CoTaskMemFree(wstr);
                }

                // Symbolic link (contains VID/PID)
                if (SUCCEEDED(
                        act->GetAllocatedString(
                            &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                            &wstr, &len)) &&
                    wstr) {
                    char buf[512] = {};
                    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, buf,
                                        sizeof(buf) - 1, nullptr, nullptr);
                    dev.device_path = buf;
                    parse_vid_pid(dev.device_path, dev.vid, dev.pid);
                    CoTaskMemFree(wstr);
                }

                if (!dev.name.empty()) devices.push_back(dev);
            }
        }
    }

    attrs->Release();
mf_cleanup:
    MFShutdown();
    CoUninitialize();
    return devices;
}

// ══════════════════════════════════════════════════════════════════════════════
// Camera Enumeration — DirectShow
// ══════════════════════════════════════════════════════════════════════════════

// Read a string property from IPropertyBag
static std::string read_property_bag(IPropertyBag* bag, const wchar_t* name) {
    VARIANT var;
    VariantInit(&var);
    std::string result;

    if (SUCCEEDED(bag->Read(name, &var, nullptr)) && var.vt == VT_BSTR &&
        var.bstrVal) {
        char buf[512] = {};
        WideCharToMultiByte(CP_UTF8, 0, var.bstrVal, -1, buf, sizeof(buf) - 1,
                            nullptr, nullptr);
        result = buf;
    }
    VariantClear(&var);
    return result;
}

static std::vector<CameraDevice> enumerate_dshow_devices() {
    std::vector<CameraDevice> devices;

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return devices;

    ICreateDevEnum* dev_enum = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr,
                                CLSCTX_INPROC_SERVER, IID_ICreateDevEnum,
                                (void**)&dev_enum))) {
        CoUninitialize();
        return devices;
    }

    IEnumMoniker* enum_moniker = nullptr;
    if (FAILED(dev_enum->CreateClassEnumerator(
            CLSID_VideoInputDeviceCategory, &enum_moniker, 0)) ||
        !enum_moniker) {
        dev_enum->Release();
        CoUninitialize();
        return devices;
    }

    IMoniker* monikers[1] = {};
    ULONG fetched = 0;
    while (enum_moniker->Next(1, monikers, &fetched) == S_OK && fetched > 0) {
        IMoniker* moniker = monikers[0];
        IPropertyBag* bag = nullptr;

        if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr,
                                              IID_IPropertyBag,
                                              (void**)&bag)) &&
            bag) {
            CameraDevice dev;
            dev.name = read_property_bag(bag, L"FriendlyName");
            dev.manufacturer = read_property_bag(bag, L"Manufacturer");
            dev.device_path = read_property_bag(bag, L"DevicePath");
            dev.driver = read_property_bag(bag, L"Driver");
            dev.clsid = read_property_bag(bag, L"CLSID");

            parse_vid_pid(dev.device_path, dev.vid, dev.pid);

            if (!dev.name.empty()) devices.push_back(dev);
            bag->Release();
        }
        moniker->Release();
    }

    enum_moniker->Release();
    dev_enum->Release();
    CoUninitialize();
    return devices;
}

// ══════════════════════════════════════════════════════════════════════════════
// Combined Camera Detection
// ══════════════════════════════════════════════════════════════════════════════
static CameraStatus detect_camera_status() {
    auto mf_devs = enumerate_mf_devices();
    auto ds_devs = enumerate_dshow_devices();

    // Deduplicate by name
    std::vector<CameraDevice> all;
    for (auto& d : mf_devs) all.push_back(std::move(d));
    for (auto& d : ds_devs) {
        bool dup = false;
        for (const auto& existing : all) {
            if (existing.name == d.name) { dup = true; break; }
        }
        if (!dup) all.push_back(std::move(d));
    }

    if (all.empty()) return CameraStatus::NoCamera;

    bool has_real = false, has_virtual = false;
    for (const auto& dev : all) {
        if (dev.name == "Unknown Camera") continue;
        if (is_virtual_camera(dev))
            has_virtual = true;
        else
            has_real = true;
    }

    if (has_real) return CameraStatus::RealCamera;
    if (has_virtual) return CameraStatus::VirtualCamera;
    return CameraStatus::NoCamera;
}

static const char* status_to_string(CameraStatus s) {
    switch (s) {
    case CameraStatus::RealCamera:    return "real_camera";
    case CameraStatus::VirtualCamera: return "virtual_camera";
    case CameraStatus::NoCamera:      return "no_camera";
    default:                          return "unknown";
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// SHA1 (using Windows CryptoAPI)
// ══════════════════════════════════════════════════════════════════════════════
static std::string sha1_hash(const std::string& input) {
    HCRYPTPROV  prov  = 0;
    HCRYPTHASH  hash  = 0;
    std::string result;

    if (!CryptAcquireContextA(&prov, nullptr, nullptr, PROV_RSA_FULL,
                              CRYPT_VERIFYCONTEXT))
        return result;

    if (CryptCreateHash(prov, CALG_SHA1, 0, 0, &hash)) {
        if (CryptHashData(hash, (const BYTE*)input.data(),
                          (DWORD)input.size(), 0)) {
            BYTE  raw[SHA1_HASH_LEN] = {};
            DWORD len = SHA1_HASH_LEN;
            if (CryptGetHashParam(hash, HP_HASHVAL, raw, &len, 0)) {
                result.assign((char*)raw, len);
            }
        }
        CryptDestroyHash(hash);
    }
    CryptReleaseContext(prov, 0);
    return result;
}

// ══════════════════════════════════════════════════════════════════════════════
// Base64 encode (using Windows CryptoAPI)
// ══════════════════════════════════════════════════════════════════════════════
static std::string base64_encode(const std::string& data) {
    DWORD out_len = 0;
    if (!CryptBinaryToStringA((const BYTE*)data.data(), (DWORD)data.size(),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              nullptr, &out_len))
        return "";

    std::string result(out_len, '\0');
    if (!CryptBinaryToStringA((const BYTE*)data.data(), (DWORD)data.size(),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              result.data(), &out_len))
        return "";
    result.resize(out_len);
    return result;
}

// ══════════════════════════════════════════════════════════════════════════════
// WebSocket accept key: base64(sha1(client_key + WS_GUID))
// ══════════════════════════════════════════════════════════════════════════════
static std::string compute_ws_accept(const std::string& client_key) {
    return base64_encode(sha1_hash(client_key + WS_GUID));
}

// ══════════════════════════════════════════════════════════════════════════════
// JSON message builder
// ══════════════════════════════════════════════════════════════════════════════
static std::string build_status_json(const char* status) {
    // Get UTC timestamp
    SYSTEMTIME st;
    GetSystemTime(&st);
    char ts[64];
    snprintf(ts, sizeof(ts), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);

    // Manual JSON: {"status":"<status>","timestamp":"<ts>"}
    char json[256];
    snprintf(json, sizeof(json),
             R"({"status":"%s","timestamp":"%s"})", status, ts);
    return json;
}

// ══════════════════════════════════════════════════════════════════════════════
// WebSocket frame encoding (server → client, unmasked)
// ══════════════════════════════════════════════════════════════════════════════
static std::string ws_encode_frame(const std::string& payload, bool text) {
    std::string frame;
    frame.reserve(payload.size() + 10);

    // FIN=1, opcode=1(text) or 9(ping)
    frame.push_back((char)(0x80 | (text ? 0x01 : 0x09)));

    // Mask=0 (server → client), payload length
    size_t len = payload.size();
    if (len <= 125) {
        frame.push_back((char)len);
    } else if (len <= 65535) {
        frame.push_back(126);
        frame.push_back((char)((len >> 8) & 0xFF));
        frame.push_back((char)(len & 0xFF));
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; --i)
            frame.push_back((char)((len >> (i * 8)) & 0xFF));
    }

    frame += payload;
    return frame;
}

// ══════════════════════════════════════════════════════════════════════════════
// WebSocket frame decoding (client → server, masked)
// ══════════════════════════════════════════════════════════════════════════════
static std::string ws_decode_frame(const char* data, size_t size,
                                   bool* is_close, bool* is_pong) {
    *is_close = false;
    *is_pong  = false;
    if (size < 2) return "";

    size_t pos = 0;
    int    opcode = data[pos] & 0x0F;
    bool   masked = (data[pos + 1] & 0x80) != 0;

    if (opcode == 0x08) { *is_close = true; return ""; }  // close
    if (opcode == 0x0A) { *is_pong = true; return ""; }   // pong
    if (opcode == 0x09) { *is_pong = true; return ""; }   // ping → treat as pong

    pos += 2;
    size_t payload_len = data[1] & 0x7F;
    if (payload_len == 126) {
        if (size < pos + 2) return "";
        payload_len = ((unsigned char)data[pos] << 8) |
                       (unsigned char)data[pos + 1];
        pos += 2;
    } else if (payload_len == 127) {
        if (size < pos + 8) return "";
        payload_len = 0;
        for (int i = 0; i < 8; ++i)
            payload_len = (payload_len << 8) | (unsigned char)data[pos + i];
        pos += 8;
    }

    char mask[4] = {};
    if (masked) {
        if (size < pos + 4) return "";
        memcpy(mask, data + pos, 4);
        pos += 4;
    }

    if (size < pos + payload_len) return "";

    std::string payload(payload_len, '\0');
    for (size_t i = 0; i < payload_len; ++i) {
        payload[i] = masked ? data[pos + i] ^ mask[i % 4] : data[pos + i];
    }

    return payload;
}

// ══════════════════════════════════════════════════════════════════════════════
// HTTP request parser — extract header value
// ══════════════════════════════════════════════════════════════════════════════
static std::string get_http_header(const std::string& request,
                                   const std::string& key) {
    std::string search = key + ": ";
    size_t pos = request.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    size_t end = request.find("\r\n", pos);
    if (end == std::string::npos) return "";
    return request.substr(pos, end - pos);
}

// ══════════════════════════════════════════════════════════════════════════════
// WebSocket Server
// ══════════════════════════════════════════════════════════════════════════════
struct WsClient {
    SOCKET fd;
    bool   upgraded;      // WebSocket handshake completed
    bool   close_pending; // pending removal
};

class WebSocketServer {
public:
    WebSocketServer(int port) : port_(port), running_(false) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }

    ~WebSocketServer() {
        stop();
        WSACleanup();
    }

    void start() {
        running_ = true;
        listen_fd_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_fd_ == INVALID_SOCKET) {
            printf("ERROR: Failed to create socket\n");
            return;
        }

        // Set non-blocking
        u_long mode = 1;
        ioctlsocket(listen_fd_, FIONBIO, &mode);

        // Allow address reuse
        BOOL reuse = TRUE;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse,
                   sizeof(reuse));

        sockaddr_in addr = {};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons((u_short)port_);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1 only

        if (bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            printf("ERROR: Failed to bind port %d\n", port_);
            closesocket(listen_fd_);
            return;
        }

        if (listen(listen_fd_, SOMAXCONN) == SOCKET_ERROR) {
            printf("ERROR: Failed to listen\n");
            closesocket(listen_fd_);
            return;
        }

        printf("WebSocket server listening on ws://127.0.0.1:%d/ws\n", port_);
        printf("Connect to ws://127.0.0.1:%d/ws to receive camera status\n", port_);

        event_loop();
    }

    void stop() {
        running_ = false;
        if (listen_fd_ != INVALID_SOCKET) {
            closesocket(listen_fd_);
            listen_fd_ = INVALID_SOCKET;
        }
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (auto& c : clients_) {
            closesocket(c.fd);
        }
        clients_.clear();
    }

    void broadcast(const std::string& message) {
        std::string frame = ws_encode_frame(message, true); // text frame
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (auto& c : clients_) {
            if (!c.upgraded || c.close_pending) continue;
            send(c.fd, frame.data(), (int)frame.size(), 0);
        }
    }

private:
    void event_loop() {
        while (running_) {
            // Build fd_set
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(listen_fd_, &read_fds);

            SOCKET max_fd = listen_fd_;
            {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                for (const auto& c : clients_) {
                    if (!c.close_pending) {
                        FD_SET(c.fd, &read_fds);
                        if (c.fd > max_fd) max_fd = c.fd;
                    }
                }
            }

            // Timeout: 100ms for responsive shutdown
            timeval tv = {0, 100000};
            int ret = select((int)max_fd + 1, &read_fds, nullptr, nullptr, &tv);
            if (ret <= 0) {
                // Cleanup disconnected clients
                cleanup_clients();
                continue;
            }

            // Accept new connections
            if (FD_ISSET(listen_fd_, &read_fds)) {
                sockaddr_in client_addr = {};
                int addr_len = sizeof(client_addr);
                SOCKET client_fd =
                    accept(listen_fd_, (sockaddr*)&client_addr, &addr_len);
                if (client_fd != INVALID_SOCKET) {
                    u_long mode = 1;
                    ioctlsocket(client_fd, FIONBIO, &mode);
                    {
                        std::lock_guard<std::mutex> lock(clients_mutex_);
                        if (clients_.size() < MAX_CLIENTS) {
                            clients_.push_back({client_fd, false, false});
                        } else {
                            const char* resp =
                                "HTTP/1.1 503 Service Unavailable\r\n"
                                "Content-Length: 0\r\n\r\n";
                            send(client_fd, resp, (int)strlen(resp), 0);
                            closesocket(client_fd);
                        }
                    }
                }
            }

            // Handle client data
            {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                for (auto& c : clients_) {
                    if (c.close_pending) continue;
                    if (!FD_ISSET(c.fd, &read_fds)) continue;

                    char buf[RECV_BUFFER_SIZE];
                    int n = recv(c.fd, buf, sizeof(buf) - 1, 0);
                    if (n <= 0) {
                        c.close_pending = true;
                        continue;
                    }
                    buf[n] = '\0';

                    if (!c.upgraded) {
                        handle_http_upgrade(c, std::string(buf, n));
                    } else {
                        handle_ws_frame(c, buf, n);
                    }
                }
            }
            cleanup_clients();
        }
    }

    void handle_http_upgrade(WsClient& c, const std::string& request) {
        // Check if this is a WebSocket upgrade request for /ws
        if (request.find("Upgrade: websocket") == std::string::npos ||
            request.find("GET /ws") == std::string::npos) {
            const char* resp =
                "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
            send(c.fd, resp, (int)strlen(resp), 0);
            c.close_pending = true;
            return;
        }

        std::string ws_key = get_http_header(request, "Sec-WebSocket-Key");
        if (ws_key.empty()) {
            const char* resp =
                "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
            send(c.fd, resp, (int)strlen(resp), 0);
            c.close_pending = true;
            return;
        }

        std::string accept_key = compute_ws_accept(ws_key);

        char response[512];
        snprintf(response, sizeof(response),
                 "HTTP/1.1 101 Switching Protocols\r\n"
                 "Upgrade: websocket\r\n"
                 "Connection: Upgrade\r\n"
                 "Sec-WebSocket-Accept: %s\r\n"
                 "\r\n",
                 accept_key.c_str());

        send(c.fd, response, (int)strlen(response), 0);
        c.upgraded = true;
    }

    void handle_ws_frame(WsClient& c, const char* data, size_t size) {
        bool is_close = false, is_pong = false;
        std::string payload =
            ws_decode_frame(data, size, &is_close, &is_pong);

        if (is_close) {
            // Send close frame and mark for removal
            char close_frame[] = {(char)0x88, (char)0x00};
            send(c.fd, close_frame, 2, 0);
            c.close_pending = true;
            return;
        }

        if (is_pong) return; // ignore pongs

        // Echo received text (optional; ignore empty)
        if (!payload.empty()) {
            std::string echo = ws_encode_frame(payload, true);
            send(c.fd, echo.data(), (int)echo.size(), 0);
        }
    }

    void cleanup_clients() {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_.erase(
            std::remove_if(clients_.begin(), clients_.end(),
                           [](const WsClient& c) { return c.close_pending; }),
            clients_.end());
    }

    int            port_;
    SOCKET         listen_fd_ = INVALID_SOCKET;
    std::atomic<bool> running_;
    std::mutex         clients_mutex_;
    std::vector<WsClient> clients_;
};

// ══════════════════════════════════════════════════════════════════════════════
// Main
// ══════════════════════════════════════════════════════════════════════════════

static void print_usage() {
    printf("cameraSimpleDetect v0.2.0 (native C++ — zero dependencies)\n");
    printf("Usage: cameraSimpleDetect.exe [--port PORT] [--interval MS]\n");
    printf("  --port     PORT   Server port (default: %d)\n", DEFAULT_PORT);
    printf("  --interval MS     Detection interval in ms (default: %d)\n",
           DEFAULT_INTERVAL_MS);
    printf("  --help            Show this help\n");
}

int main(int argc, char* argv[]) {
    int port     = DEFAULT_PORT;
    int interval = DEFAULT_INTERVAL_MS;

    // Parse command-line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        }
        if ((arg == "--port" || arg == "-p") && i + 1 < argc) {
            port = atoi(argv[++i]);
            if (port <= 0 || port > 65535) {
                printf("ERROR: Invalid port number\n");
                return 1;
            }
        } else if ((arg == "--interval" || arg == "-i") && i + 1 < argc) {
            interval = atoi(argv[++i]);
            if (interval < 500) {
                printf("WARNING: Interval too low, setting to 500ms\n");
                interval = 500;
            }
        }
    }

    printf("cameraSimpleDetect v0.2.0 (native C++)\n");
    printf("Port: %d, Interval: %dms\n", port, interval);

    WebSocketServer server(port);

    // Start monitor thread
    std::atomic<bool>         monitor_running{true};
    CameraStatus              last_status = CameraStatus::Unknown;
    std::mutex                status_mutex;
    std::vector<std::string>  pending_messages;
    std::mutex                pending_mutex;

    std::thread monitor([&]() {
        printf("[Monitor] Camera detection started\n");
        while (monitor_running) {
            CameraStatus current = detect_camera_status();

            bool changed = false;
            {
                std::lock_guard<std::mutex> lock(status_mutex);
                if (current != last_status) {
                    last_status = current;
                    changed = true;
                }
            }

            if (changed) {
                const char* status_str = status_to_string(current);
                printf("[Detect] Status changed: %s\n", status_str);
                std::string json = build_status_json(status_str);
                server.broadcast(json);
            }

            // Sleep with periodic check
            int slept = 0;
            while (slept < interval && monitor_running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                slept += 100;
            }
        }
        printf("[Monitor] Stopped\n");
    });

    // Run event loop (blocks until Ctrl+C or server stop)
    printf("Press Ctrl+C to stop\n\n");
    server.start();

    // Cleanup
    monitor_running = false;
    if (monitor.joinable()) monitor.join();

    printf("Server stopped.\n");
    return 0;
}
