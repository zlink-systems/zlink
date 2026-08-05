using System.Net;
using System.Net.Sockets;

namespace SpotService.Client.Support;

internal sealed class ReconnectProxy : IAsyncDisposable
{
    private readonly CancellationTokenSource _stop = new();
    private readonly TcpListener _listener = new(IPAddress.Loopback, 0);
    private readonly Uri _upstream;
    private readonly SemaphoreSlim _accepted = new(0);
    private readonly object _gate = new();
    private readonly Task _run;
    private TcpClient? _client;
    private TcpClient? _server;

    public ReconnectProxy(Uri upstream)
    {
        _upstream = upstream;
        _listener.Start();
        Endpoint = new Uri($"tcp://127.0.0.1:{((IPEndPoint)_listener.LocalEndpoint).Port}");
        _run = RunAsync(_stop.Token);
    }

    public Uri Endpoint { get; }

    public void DropConnection()
    {
        lock (_gate)
        {
            _client?.Dispose();
            _server?.Dispose();
        }
    }

    public async Task WaitForConnectionAsync(CancellationToken cancellationToken = default)
    {
        if (!await _accepted.WaitAsync(TimeSpan.FromSeconds(10), cancellationToken))
            throw new TimeoutException("Reconnect proxy did not accept a connection.");
    }

    private async Task RunAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            using var client = await _listener.AcceptTcpClientAsync(cancellationToken);
            using var server = new TcpClient();
            await server.ConnectAsync(_upstream.Host, _upstream.Port, cancellationToken);
            lock (_gate) { _client = client; _server = server; }
            _accepted.Release();
            try
            {
                await Task.WhenAny(
                    PumpAsync(client.GetStream(), server.GetStream(), cancellationToken),
                    PumpAsync(server.GetStream(), client.GetStream(), cancellationToken));
            }
            catch (Exception error) when (error is IOException or ObjectDisposedException or SocketException)
            {
            }
            finally
            {
                lock (_gate) { _client = null; _server = null; }
            }
        }
    }

    private static async Task PumpAsync(NetworkStream source, NetworkStream target,
        CancellationToken cancellationToken)
    {
        var buffer = new byte[16 * 1024];
        while (true)
        {
            var count = await source.ReadAsync(buffer, cancellationToken);
            if (count == 0) return;
            await target.WriteAsync(buffer.AsMemory(0, count), cancellationToken);
        }
    }

    public async ValueTask DisposeAsync()
    {
        _stop.Cancel();
        _listener.Stop();
        DropConnection();
        try { await _run; }
        catch (Exception error) when (error is OperationCanceledException or ObjectDisposedException) { }
        _accepted.Dispose();
        _stop.Dispose();
    }
}
