using System.Net;
using System.Net.Sockets;

namespace Zlink.Framework.UnitTests.Runtime;

// Opaque TCP forwarding lets a submitted record remain on the retiring pipe.
// The test never decodes Core frames or chooses a route.
internal sealed class RouteChangeProxy : IAsyncDisposable
{
    private const int CopyBufferSize = 16 * 1024;
    private readonly TcpListener _listener;
    private Uri _upstream = null!;
    private readonly CancellationTokenSource _stop = new();
    private readonly TaskCompletionSource _resume = new(
        TaskCreationOptions.RunContinuationsAsynchronously
    );
    private Task _forward = Task.CompletedTask;
    private readonly Task _relay;
    private readonly List<Task> _connections = [];

    internal RouteChangeProxy()
    {
        _listener = new TcpListener(IPAddress.Loopback, 0);
        _listener.Start();
        Endpoint = $"tcp://{_listener.LocalEndpoint}";
        _relay = AcceptAsync();
    }

    internal string Endpoint { get; }

    internal void ForwardTo(string upstream) => _upstream = new Uri(upstream);

    internal void Pause() => Volatile.Write(ref _forward, _resume.Task);

    internal void Resume() => _resume.TrySetResult();

    private async Task AcceptAsync()
    {
        try
        {
            while (!_stop.IsCancellationRequested)
                _connections.Add(
                    RelayAsync(
                        await _listener.AcceptTcpClientAsync(_stop.Token).ConfigureAwait(false)
                    )
                );
        }
        catch (OperationCanceledException) when (_stop.IsCancellationRequested) { }
        catch (SocketException) when (_stop.IsCancellationRequested) { }
    }

    private async Task RelayAsync(TcpClient downstream)
    {
        using var ownedDownstream = downstream;
        try
        {
            using var upstream = new TcpClient();
            await upstream
                .ConnectAsync(_upstream.Host, _upstream.Port, _stop.Token)
                .ConfigureAwait(false);
            await Task.WhenAll(
                    CopyAsync(downstream.GetStream(), upstream.GetStream(), gated: false),
                    CopyAsync(upstream.GetStream(), downstream.GetStream(), gated: true)
                )
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (_stop.IsCancellationRequested) { }
        catch (IOException)
            when (_stop.IsCancellationRequested
                || ReferenceEquals(Volatile.Read(ref _forward), _resume.Task)
            )
        { /* A retired pipe may close while bytes are held. */
        }
        catch (SocketException) when (_stop.IsCancellationRequested) { }
    }

    private async Task CopyAsync(NetworkStream source, NetworkStream target, bool gated)
    {
        var buffer = new byte[CopyBufferSize];
        while (true)
        {
            var count = await source.ReadAsync(buffer, _stop.Token).ConfigureAwait(false);
            if (count == 0)
                return;
            if (gated)
                await Volatile.Read(ref _forward).WaitAsync(_stop.Token).ConfigureAwait(false);
            await target.WriteAsync(buffer.AsMemory(0, count), _stop.Token).ConfigureAwait(false);
        }
    }

    public async ValueTask DisposeAsync()
    {
        await _stop.CancelAsync().ConfigureAwait(false);
        _listener.Stop();
        await _relay.ConfigureAwait(false);
        await Task.WhenAll(_connections).ConfigureAwait(false);
        _stop.Dispose();
    }
}
