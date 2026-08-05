using System.Net;
using System.Net.Sockets;

namespace PubSub.Client.Support;

internal sealed class NetworkFaultProxy : IAsyncDisposable
{
    private readonly CancellationTokenSource _stop = new();
    private readonly TcpListener _listener = new(IPAddress.Loopback, 0);
    private readonly Uri _upstream;
    private readonly SemaphoreSlim _networkGate = new(1, 1);
    private readonly SemaphoreSlim _connected = new(0);
    private readonly object _connectionGate = new();
    private readonly Task _run;
    private TcpClient? _client;
    private TcpClient? _server;
    private bool _blocked;
    private long _clientToServerBytes;
    private long _serverToClientBytes;
    private string? _lastPumpFailure;

    public NetworkFaultProxy(Uri upstream, bool initiallyBlocked = false)
    {
        _upstream = upstream;
        if (initiallyBlocked)
        {
            _networkGate.Wait();
            _blocked = true;
        }
        _listener.Start();
        Endpoint = new Uri($"tcp://127.0.0.1:{((IPEndPoint)_listener.LocalEndpoint).Port}");
        _run = RunAsync(_stop.Token);
    }

    public Uri Endpoint { get; }

    public string Diagnostics =>
        $"clientToServer={Interlocked.Read(ref _clientToServerBytes)} "
        + $"serverToClient={Interlocked.Read(ref _serverToClientBytes)} "
        + $"lastFailure={_lastPumpFailure ?? "<none>"}";

    public void Block()
    {
        lock (_connectionGate)
        {
            if (_blocked) return;
            if (!_networkGate.Wait(0))
                throw new InvalidOperationException("The network fault proxy gate is already held.");
            _blocked = true;
            _client?.Dispose();
            _server?.Dispose();
        }
    }

    public void Unblock()
    {
        lock (_connectionGate)
        {
            if (!_blocked) return;
            _blocked = false;
            _networkGate.Release();
        }
    }

    public async Task WaitForUpstreamConnectionAsync(CancellationToken cancellationToken = default)
    {
        if (!await _connected.WaitAsync(TimeSpan.FromSeconds(15), cancellationToken))
            throw new TimeoutException("The network fault proxy did not connect to its upstream endpoint.");
    }

    private async Task RunAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            using var client = await _listener.AcceptTcpClientAsync(cancellationToken);
            client.NoDelay = true;
            await _networkGate.WaitAsync(cancellationToken);
            _networkGate.Release();
            using var server = new TcpClient();
            server.NoDelay = true;
            await server.ConnectAsync(_upstream.Host, _upstream.Port, cancellationToken);
            lock (_connectionGate)
            {
                _client = client;
                _server = server;
            }
            _connected.Release();
            try
            {
                await Task.WhenAny(
                    PumpAsync(client.GetStream(), server.GetStream(), true, cancellationToken),
                    PumpAsync(server.GetStream(), client.GetStream(), false, cancellationToken));
            }
            catch (Exception error) when (
                error is IOException or ObjectDisposedException or SocketException)
            {
                _lastPumpFailure = $"{error.GetType().Name}:{error.Message}";
            }
            finally
            {
                lock (_connectionGate)
                {
                    _client = null;
                    _server = null;
                }
            }
        }
    }

    private async Task PumpAsync(
        NetworkStream source,
        NetworkStream target,
        bool clientToServer,
        CancellationToken cancellationToken)
    {
        var buffer = new byte[16 * 1024];
        while (true)
        {
            var count = await source.ReadAsync(buffer, cancellationToken);
            if (count == 0) return;
            await target.WriteAsync(buffer.AsMemory(0, count), cancellationToken);
            if (clientToServer)
                Interlocked.Add(ref _clientToServerBytes, count);
            else
                Interlocked.Add(ref _serverToClientBytes, count);
        }
    }

    public async ValueTask DisposeAsync()
    {
        Unblock();
        _stop.Cancel();
        _listener.Stop();
        lock (_connectionGate)
        {
            _client?.Dispose();
            _server?.Dispose();
        }
        try
        {
            await _run;
        }
        catch (Exception error) when (
            error is OperationCanceledException or ObjectDisposedException or SocketException)
        {
        }
        _connected.Dispose();
        _networkGate.Dispose();
        _stop.Dispose();
    }
}
