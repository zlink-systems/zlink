using System.Net;
using System.Net.Sockets;

var listenPort = RequireInt(args, "--listen-port");
var targetPort = RequireInt(args, "--target-port");
var httpUrl = RequireString(args, "--http-url");

var builder = WebApplication.CreateBuilder(args);
builder.Configuration.Sources.Clear();
builder.Configuration.AddInMemoryCollection();
builder.Logging.ClearProviders();
builder.WebHost.UseUrls(httpUrl);

await using var gate = new ReceiverGate(listenPort, targetPort, 4096);
gate.Start();

var app = builder.Build();
app.MapGet("/health", () => Results.Ok(gate.Snapshot()));
app.MapPost("/gate/close", async () => Results.Ok(await gate.CloseAsync()));
app.MapPost("/gate/open", () =>
{
    gate.Open();
    return Results.Ok(gate.Snapshot());
});
app.MapGet("/gate/status", () => Results.Ok(gate.Snapshot()));
await app.RunAsync();

static string RequireString(string[] args, string name)
{
    var index = Array.IndexOf(args, name);
    if (index < 0 || index + 1 >= args.Length)
        throw new ArgumentException($"{name} is required.");
    return args[index + 1];
}

static int RequireInt(string[] args, string name) =>
    int.TryParse(RequireString(args, name), out var value) && value is > 0 and <= 65535
        ? value
        : throw new ArgumentOutOfRangeException(name);

internal sealed record ReceiverGateSnapshot(
    bool Open,
    bool Connected,
    long CallerBytesRead,
    long ClosedAtCallerBytes,
    long CallerBytesReadAfterClose,
    int RequestedSocketBufferBytes,
    int CallerReceiveBufferBytes,
    int CallerSendBufferBytes,
    int TargetReceiveBufferBytes,
    int TargetSendBufferBytes);

internal sealed class ReceiverGate : IAsyncDisposable
{
    private readonly CancellationTokenSource _stop = new();
    private readonly object _stateGate = new();
    private readonly TcpListener _listener;
    private readonly int _targetPort;
    private readonly int _socketBufferBytes;
    private CancellationTokenSource _readEnabled = new();
    private TaskCompletionSource _paused = CompletedSignal();
    private TaskCompletionSource _resumed = CompletedSignal();
    private readonly List<Socket> _live = [];
    private Task? _run;
    private Socket? _caller;
    private Socket? _target;
    private bool _open = true;
    private long _callerBytesRead;
    private long _closedAtCallerBytes;

    public ReceiverGate(int listenPort, int targetPort, int socketBufferBytes)
    {
        _targetPort = targetPort;
        _socketBufferBytes = socketBufferBytes;
        _listener = new TcpListener(IPAddress.Loopback, listenPort);
    }

    public void Start()
    {
        // Apply the receive-window request before the TCP handshake so the
        // caller never observes a large initial advertised window.
        Configure(_listener.Server);
        _listener.Start();
        _run = RunAsync();
    }

    public async Task<ReceiverGateSnapshot> CloseAsync()
    {
        Task paused;
        lock (_stateGate)
        {
            if (!_open) return Snapshot();
            _open = false;
            _paused = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            _resumed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            paused = _paused.Task;
            _readEnabled.Cancel();
        }

        await paused.WaitAsync(TimeSpan.FromSeconds(1));
        Interlocked.Exchange(ref _closedAtCallerBytes, Interlocked.Read(ref _callerBytesRead));
        return Snapshot();
    }

    public void Open()
    {
        lock (_stateGate)
        {
            if (_open) return;
            _readEnabled.Dispose();
            _readEnabled = new CancellationTokenSource();
            _open = true;
            _resumed.TrySetResult();
        }
    }

    public ReceiverGateSnapshot Snapshot()
    {
        var callerBytes = Interlocked.Read(ref _callerBytesRead);
        var closedAt = Interlocked.Read(ref _closedAtCallerBytes);
        lock (_stateGate)
        {
            return new ReceiverGateSnapshot(
                _open,
                _caller is not null && _target is not null,
                callerBytes,
                closedAt,
                _open ? 0 : callerBytes - closedAt,
                _socketBufferBytes,
                _caller?.ReceiveBufferSize ?? 0,
                _caller?.SendBufferSize ?? 0,
                _target?.ReceiveBufferSize ?? 0,
                _target?.SendBufferSize ?? 0);
        }
    }

    public async ValueTask DisposeAsync()
    {
        _stop.Cancel();
        lock (_stateGate)
        {
            _readEnabled.Cancel();
            _resumed.TrySetResult();
        }
        _listener.Stop();
        lock (_stateGate)
            foreach (var socket in _live)
                socket.Dispose();
        if (_run is not null)
        {
            try { await _run.ConfigureAwait(false); }
            catch (OperationCanceledException) { }
            catch (ObjectDisposedException) { }
            catch (SocketException) when (_stop.IsCancellationRequested) { }
        }
        _readEnabled.Dispose();
        _stop.Dispose();
    }

    private async Task RunAsync()
    {
        // A mesh peer is a transport pair: one connection carries application
        // traffic and a second carries handshake, liveness and replies.
        // Accepting only the first leaves the peer permanently half-connected,
        // so every connection gets its own forwarding pair.
        var pairs = new List<Task>();
        try
        {
            while (!_stop.IsCancellationRequested)
            {
                var caller = await _listener.AcceptSocketAsync(_stop.Token).ConfigureAwait(false);
                pairs.Add(ForwardPairAsync(caller));
            }
        }
        catch (OperationCanceledException) when (_stop.IsCancellationRequested)
        {
        }

        await Task.WhenAll(pairs).ConfigureAwait(false);
    }

    private async Task ForwardPairAsync(Socket caller)
    {
        Configure(caller);
        var target = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
        Configure(target);
        await target.ConnectAsync(IPAddress.Loopback, _targetPort, _stop.Token).ConfigureAwait(false);
        lock (_stateGate)
        {
            _caller ??= caller;
            _target ??= target;
            _live.Add(caller);
            _live.Add(target);
        }

        try
        {
            await Task.WhenAll(
                    ForwardCallerAsync(caller, target),
                    ForwardAsync(target, caller))
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (_stop.IsCancellationRequested)
        {
        }
        catch (SocketException)
        {
        }
        catch (ObjectDisposedException)
        {
        }
    }

    private async Task ForwardCallerAsync(Socket source, Socket destination)
    {
        var buffer = new byte[_socketBufferBytes];
        while (!_stop.IsCancellationRequested)
        {
            CancellationToken readEnabled;
            lock (_stateGate)
            {
                readEnabled = _readEnabled.Token;
            }

            try
            {
                var read = await source.ReceiveAsync(buffer, SocketFlags.None, readEnabled)
                    .ConfigureAwait(false);
                if (read == 0) return;
                Interlocked.Add(ref _callerBytesRead, read);
                await SendAllAsync(destination, buffer.AsMemory(0, read), _stop.Token)
                    .ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (!_stop.IsCancellationRequested)
            {
                _paused.TrySetResult();
                Task resumed;
                lock (_stateGate) resumed = _resumed.Task;
                await resumed.WaitAsync(_stop.Token).ConfigureAwait(false);
            }
        }
    }

    private async Task ForwardAsync(Socket source, Socket destination)
    {
        var buffer = new byte[_socketBufferBytes];
        while (!_stop.IsCancellationRequested)
        {
            var read = await source.ReceiveAsync(buffer, SocketFlags.None, _stop.Token)
                .ConfigureAwait(false);
            if (read == 0) return;
            await SendAllAsync(destination, buffer.AsMemory(0, read), _stop.Token)
                .ConfigureAwait(false);
        }
    }

    private static async Task SendAllAsync(
        Socket destination,
        ReadOnlyMemory<byte> bytes,
        CancellationToken cancellationToken)
    {
        while (!bytes.IsEmpty)
        {
            var sent = await destination.SendAsync(bytes, SocketFlags.None, cancellationToken)
                .ConfigureAwait(false);
            if (sent == 0) throw new IOException("ReceiverGate destination closed during forwarding.");
            bytes = bytes[sent..];
        }
    }

    private void Configure(Socket socket)
    {
        socket.ReceiveBufferSize = _socketBufferBytes;
        socket.SendBufferSize = _socketBufferBytes;
        socket.NoDelay = true;
    }

    private static TaskCompletionSource CompletedSignal()
    {
        var source = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        source.SetResult();
        return source;
    }
}
