using System.Collections.Concurrent;
using System.Management;
using System.Net;
using System.Net.WebSockets;
using System.Text;
using System.Text.Json;

const int DefaultPort = 8787;
const int DefaultIntervalMs = 2000;

var port = GetIntFromEnvironment("CAMERA_WS_PORT", DefaultPort);
var intervalMs = GetIntFromEnvironment("CAMERA_DETECT_INTERVAL_MS", DefaultIntervalMs);

using var cts = new CancellationTokenSource();
Console.CancelKeyPress += (_, e) =>
{
    e.Cancel = true;
    cts.Cancel();
};

var server = new WebSocketServer(port);
var detector = new CameraDetector();

var serverTask = server.StartAsync(cts.Token);
var monitorTask = MonitorAsync(detector, server, intervalMs, cts.Token);

await Task.WhenAll(serverTask, monitorTask);

static async Task MonitorAsync(CameraDetector detector, WebSocketServer server, int intervalMs, CancellationToken cancellationToken)
{
    var lastStatus = CameraStatus.Unknown;
    while (!cancellationToken.IsCancellationRequested)
    {
        var status = detector.DetectStatus();
        if (status != lastStatus)
        {
            lastStatus = status;
            await server.BroadcastAsync(new StatusMessage(status), cancellationToken);
        }

        try
        {
            await Task.Delay(intervalMs, cancellationToken);
        }
        catch (TaskCanceledException)
        {
            break;
        }
    }
}

static int GetIntFromEnvironment(string name, int fallback)
{
    var value = Environment.GetEnvironmentVariable(name);
    if (int.TryParse(value, out var parsed) && parsed > 0)
    {
        return parsed;
    }

    return fallback;
}

enum CameraStatus
{
    Unknown,
    RealCamera,
    VirtualCamera,
    NoCamera
}

sealed class StatusMessage
{
    public StatusMessage(CameraStatus status)
    {
        Status = status switch
        {
            CameraStatus.RealCamera => "real_camera",
            CameraStatus.VirtualCamera => "virtual_camera",
            CameraStatus.NoCamera => "no_camera",
            _ => "unknown"
        };
        Timestamp = DateTimeOffset.UtcNow;
    }

    public string Status { get; }
    public DateTimeOffset Timestamp { get; }
}

sealed class CameraDetector
{
    private static readonly string[] VirtualKeywords =
    {
        "virtual",
        "obs",
        "manycam",
        "snap camera",
        "xsplit",
        "v4l2",
        "droidcam",
        "iriun",
        "epoccam",
        "vcam",
        "ndi"
    };

    public CameraStatus DetectStatus()
    {
        var deviceNames = EnumerateDeviceNames();
        if (deviceNames.Count == 0)
        {
            return CameraStatus.NoCamera;
        }

        var hasVirtual = false;
        var hasReal = false;

        foreach (var name in deviceNames)
        {
            if (IsVirtual(name))
            {
                hasVirtual = true;
            }
            else
            {
                hasReal = true;
            }
        }

        if (hasReal)
        {
            return CameraStatus.RealCamera;
        }

        return hasVirtual ? CameraStatus.VirtualCamera : CameraStatus.NoCamera;
    }

    private static bool IsVirtual(string name)
    {
        var lowered = name.ToLowerInvariant();
        return VirtualKeywords.Any(keyword => lowered.Contains(keyword));
    }

    private static List<string> EnumerateDeviceNames()
    {
        var results = new List<string>();

        try
        {
            using var searcher = new ManagementObjectSearcher(
                "SELECT Name FROM Win32_PnPEntity WHERE PNPClass = 'Image'");
            using var collection = searcher.Get();
            foreach (var item in collection)
            {
                var name = item["Name"]?.ToString();
                if (!string.IsNullOrWhiteSpace(name))
                {
                    results.Add(name);
                }
            }
        }
        catch (ManagementException)
        {
            return results;
        }

        return results;
    }
}

sealed class WebSocketServer
{
    private readonly int _port;
    private readonly ConcurrentDictionary<Guid, WebSocket> _clients = new();

    public WebSocketServer(int port)
    {
        _port = port;
    }

    public async Task StartAsync(CancellationToken cancellationToken)
    {
        using var listener = new HttpListener();
        listener.Prefixes.Add($"http://127.0.0.1:{_port}/ws/");
        listener.Start();
        Console.WriteLine($"WebSocket listening on ws://127.0.0.1:{_port}/ws/");

        while (!cancellationToken.IsCancellationRequested)
        {
            HttpListenerContext context;
            try
            {
                context = await listener.GetContextAsync();
            }
            catch (HttpListenerException) when (cancellationToken.IsCancellationRequested)
            {
                break;
            }

            if (!context.Request.IsWebSocketRequest)
            {
                context.Response.StatusCode = 400;
                context.Response.Close();
                continue;
            }

            _ = HandleClientAsync(context, cancellationToken);
        }
    }

    public async Task BroadcastAsync(StatusMessage message, CancellationToken cancellationToken)
    {
        if (_clients.IsEmpty)
        {
            return;
        }

        var payload = JsonSerializer.Serialize(message);
        var buffer = Encoding.UTF8.GetBytes(payload);
        var segment = new ArraySegment<byte>(buffer);

        foreach (var entry in _clients)
        {
            var socket = entry.Value;
            if (socket.State != WebSocketState.Open)
            {
                _clients.TryRemove(entry.Key, out _);
                continue;
            }

            try
            {
                await socket.SendAsync(segment, WebSocketMessageType.Text, true, cancellationToken);
            }
            catch (WebSocketException)
            {
                _clients.TryRemove(entry.Key, out _);
            }
        }
    }

    private async Task HandleClientAsync(HttpListenerContext context, CancellationToken cancellationToken)
    {
        WebSocket webSocket;
        try
        {
            var wsContext = await context.AcceptWebSocketAsync(null);
            webSocket = wsContext.WebSocket;
        }
        catch (WebSocketException)
        {
            context.Response.StatusCode = 500;
            context.Response.Close();
            return;
        }

        var id = Guid.NewGuid();
        _clients[id] = webSocket;

        var buffer = new byte[1024];
        try
        {
            while (webSocket.State == WebSocketState.Open && !cancellationToken.IsCancellationRequested)
            {
                var result = await webSocket.ReceiveAsync(new ArraySegment<byte>(buffer), cancellationToken);
                if (result.MessageType == WebSocketMessageType.Close)
                {
                    break;
                }
            }
        }
        catch (OperationCanceledException)
        {
        }
        finally
        {
            _clients.TryRemove(id, out _);
            try
            {
                await webSocket.CloseAsync(WebSocketCloseStatus.NormalClosure, "closed", CancellationToken.None);
            }
            catch (WebSocketException)
            {
            }
        }
    }
}
