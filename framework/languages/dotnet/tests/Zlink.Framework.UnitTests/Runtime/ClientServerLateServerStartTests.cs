using System.Net;
using System.Net.Sockets;
using Microsoft.Extensions.DependencyInjection;

namespace Zlink.Framework.UnitTests;

public sealed partial class ClientServerChannelRuntimeTests
{
    [Theory]
    [InlineData(1)]
    [InlineData(2)]
    [InlineData(3)]
    [InlineData(4)]
    [InlineData(5)]
    public async Task ClientSendWaitsBeyondThreeDefaultTimeoutsAndCompletesAfterServerStarts(
        int iteration
    )
    {
        var proxy = new LateServerStartTcpProxy();
        AssertTcpEndpointIsOccupied(proxy.Endpoint);
        await using var client = CreateClient(proxy.Endpoint);
        await using var server = CreateServer(0, bindHost: AdmissionTestServerHost);
        var clientRuntime = client.GetRequiredService<ZLinkFrameworkRuntime>();
        var serverRuntime = server.GetRequiredService<ZLinkFrameworkRuntime>();
        try
        {
            await clientRuntime.StartAsync(CancellationToken.None);
            var payload = $"too-late-{iteration}";
            var send = client
                .GetRequiredService<IZLinkRouteClient>()
                .SendToChannel("work", new EchoSend(payload))
                .Async()
                .AsTask();
            await proxy.WaitForConnectionAsync(TimeSpan.FromSeconds(5));
            await Task.Delay(TimeSpan.FromMilliseconds(3100));
            Assert.False(send.IsCompleted);

            await serverRuntime.StartAsync(CancellationToken.None);
            var serverState = await serverRuntime.EnsureStartedStateAsync(CancellationToken.None);
            var serverEndpoint = (
                await GetServerBundle(serverState, "work").ClientServerServer!.ReadAsync()
            ).AdvertisedEndpoint;
            Assert.NotEqual(new Uri(proxy.Endpoint).Port, new Uri(serverEndpoint).Port);
            proxy.ForwardTo(serverEndpoint);

            var clientTransport = clientRuntime.GetClientServerClientRuntime("work");
            await WaitUntilAsync(
                clientTransport,
                () => clientTransport.ReadyCount == 1,
                TimeSpan.FromSeconds(10)
            );
            await send.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(
                payload,
                await server
                    .GetRequiredService<EchoProbe>()
                    .Received.Task.WaitAsync(TimeSpan.FromSeconds(5))
            );
            AssertTcpEndpointIsOccupied(proxy.Endpoint);
        }
        finally
        {
            try
            {
                await proxy.DisposeAsync();
            }
            finally
            {
                await serverRuntime.StopAsync(CancellationToken.None);
                await clientRuntime.StopAsync(CancellationToken.None);
            }
        }
    }

    private static void AssertTcpEndpointIsOccupied(string endpoint)
    {
        var port = new Uri(endpoint).Port;
        var competitor = new TcpListener(IPAddress.Loopback, port);
        try
        {
            var error = Assert.Throws<SocketException>(() => competitor.Start());
            Assert.Equal(SocketError.AddressAlreadyInUse, error.SocketErrorCode);
        }
        finally
        {
            competitor.Stop();
        }
    }

    private sealed class LateServerStartTcpProxy : IAsyncDisposable
    {
        private const int CopyBufferSize = 16 * 1024;
        private readonly TcpListener _listener = new(IPAddress.Loopback, 0);
        private readonly CancellationTokenSource _stop = new();
        private readonly TaskCompletionSource _accepted = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        private readonly TaskCompletionSource<Uri> _upstream = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        private readonly List<Task> _connections = [];
        private readonly Task _accept;

        internal LateServerStartTcpProxy()
        {
            _listener.Start();
            var port = ((IPEndPoint)_listener.LocalEndpoint).Port;
            Endpoint = $"tcp://127.0.0.1:{port}";
            _accept = AcceptAsync();
        }

        internal string Endpoint { get; }

        internal Task WaitForConnectionAsync(TimeSpan timeout) => _accepted.Task.WaitAsync(timeout);

        internal void ForwardTo(string endpoint)
        {
            if (!_upstream.TrySetResult(new Uri(endpoint)))
                throw new InvalidOperationException("The proxy target was already set.");
        }

        private async Task AcceptAsync()
        {
            try
            {
                while (!_stop.IsCancellationRequested)
                {
                    var downstream = await _listener
                        .AcceptTcpClientAsync(_stop.Token)
                        .ConfigureAwait(false);
                    _accepted.TrySetResult();
                    _connections.Add(RelayAsync(downstream));
                }
            }
            catch (OperationCanceledException) when (_stop.IsCancellationRequested) { }
            catch (SocketException) when (_stop.IsCancellationRequested) { }
        }

        private async Task RelayAsync(TcpClient downstream)
        {
            using (downstream)
            {
                try
                {
                    // Until the server starts, close each connection like an absent server
                    // so the client keeps going through its reconnect path.
                    if (!_upstream.Task.IsCompleted)
                        return;
                    var endpoint = await _upstream
                        .Task.WaitAsync(_stop.Token)
                        .ConfigureAwait(false);
                    using var upstream = new TcpClient();
                    await upstream
                        .ConnectAsync(endpoint.Host, endpoint.Port, _stop.Token)
                        .ConfigureAwait(false);
                    await Task.WhenAll(
                            CopyAsync(downstream.GetStream(), upstream.GetStream()),
                            CopyAsync(upstream.GetStream(), downstream.GetStream())
                        )
                        .ConfigureAwait(false);
                }
                catch (OperationCanceledException) when (_stop.IsCancellationRequested) { }
                catch (IOException) when (_stop.IsCancellationRequested) { }
                catch (SocketException) when (_stop.IsCancellationRequested) { }
            }
        }

        private async Task CopyAsync(NetworkStream source, NetworkStream target)
        {
            var buffer = new byte[CopyBufferSize];
            while (true)
            {
                var count = await source
                    .ReadAsync(buffer.AsMemory(), _stop.Token)
                    .ConfigureAwait(false);
                if (count == 0)
                    return;
                await target
                    .WriteAsync(buffer.AsMemory(0, count), _stop.Token)
                    .ConfigureAwait(false);
            }
        }

        public async ValueTask DisposeAsync()
        {
            await _stop.CancelAsync().ConfigureAwait(false);
            _listener.Stop();
            await _accept.ConfigureAwait(false);
            await Task.WhenAll(_connections).ConfigureAwait(false);
            _stop.Dispose();
        }
    }
}
