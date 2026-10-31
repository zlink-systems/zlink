using System.Diagnostics;
using System.Net;
using System.Net.Sockets;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class RemoteSpotFirstRequestTests
{
    [Fact]
    public async Task New_host_requests_existing_spot_without_waiting_for_peer_ready()
    {
        await using var owner = await SpotCloseHost.StartAsync();
        var spot = await owner.CreateSpotAsync();
        await using var requester = await SpotCloseHost.StartAsync(owner.Store);
        var existing = await requester
            .Manager.GetOrCreate(spot.SpotId, SpotCloseHost.SpotType)
            .InMesh(SpotCloseHost.MeshName)
            .Async();
        Assert.Equal(ZLinkSpotCreateState.Existing, existing.State);
        Assert.Equal("handlerReply", await requester.RequestAsync(spot.SpotId));
        Assert.Equal(1, owner.State.HandlerCalls);
        Assert.Equal(0, requester.State.HandlerCalls);
    }

    [Fact]
    public async Task Actual_connection_refusal_ends_request_before_its_deadline()
    {
        // Release the port before connecting: a bound, non-listening TCP socket
        // drops incoming SYNs on macOS instead of refusing the connection.
        string endpoint;
        using (
            var reservation = new Socket(
                AddressFamily.InterNetwork,
                System.Net.Sockets.SocketType.Stream,
                ProtocolType.Tcp
            )
        )
        {
            reservation.Bind(new IPEndPoint(IPAddress.Loopback, 0));
            endpoint = $"tcp://127.0.0.1:{((IPEndPoint)reservation.LocalEndPoint!).Port}";
        }
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var requester = new ZLinkManagedMeshNode(context, "connection-refusal");
        var target = RoutingId.From("unreachable-owner");
        requester.SetRoutingId(RoutingId.From("connection-refusal-requester"));
        requester.Start();
        requester.ConnectPeer(endpoint, target);
        using var part = Message.From("first-request");
        var elapsed = Stopwatch.StartNew();
        var error = await Assert.ThrowsAsync<ZlinkSubmitException>(async () =>
            await requester
                .RequestToNodeDirectAsync(
                    target,
                    [part],
                    SendFlags.None,
                    default,
                    TimeSpan.FromSeconds(30),
                    CancellationToken.None
                )
                .AsTask()
                .WaitAsync(TimeSpan.FromSeconds(5))
        );
        Assert.Equal(ZlinkSubmitException.ErrorCode.NotConnected, error.Result);
        Assert.True(elapsed.Elapsed < TimeSpan.FromSeconds(30));
        var attempt = requester.ObservePeerRoute(target).Attempt;
        Assert.NotNull(attempt);
        Assert.Equal(ConnectionAttemptState.AttemptFailed, attempt.Value.State);
        Assert.Equal(ConnectionFailureKind.Refused, attempt.Value.LastFailure.Kind);
        Assert.Equal(ConnectionFailureStage.Transport, attempt.Value.LastFailure.Stage);
    }

    [Fact]
    public async Task Delayed_tcp_handshake_waits_then_delivers_one_request()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var owner = new ZLinkManagedMeshNode(context, "late-tcp");
        await using var requester = new ZLinkManagedMeshNode(context, "late-tcp");
        await using var proxy = new HandshakeGateProxy();
        var target = RoutingId.From("late-tcp-owner");
        owner.SetRoutingId(target);
        owner.SetBind("tcp://127.0.0.1:0");
        owner.SetAdvertisedEndpoint(proxy.Endpoint);
        owner.Start();
        proxy.ForwardTo(owner.Status().LocalEndpoint);
        requester.SetRoutingId(RoutingId.From("late-tcp-requester"));
        requester.Start();
        requester.ConnectPeer(proxy.Endpoint, target);
        await proxy.Accepted.Task.WaitAsync(TimeSpan.FromSeconds(3));
        using var bound = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        ZLinkPeerRouteObservation connecting;
        while ((connecting = requester.ObservePeerRoute(target)).Attempt is null)
            await connecting.Changed.WaitAsync(bound.Token);
        Assert.Equal(ConnectionAttemptState.Connecting, connecting.Attempt.Value.State);
        Assert.Equal(0UL, connecting.ReadyGeneration);
        using var part = Message.From("delayed-request");
        var admission = new ZLinkRequestAdmission();
        var request = requester
            .RequestToNodeDirectAsync(
                target,
                [part],
                SendFlags.None,
                default,
                TimeSpan.FromSeconds(5),
                bound.Token,
                requestAdmission: admission
            )
            .AsTask();
        try
        {
            Assert.False(request.IsCompleted);
            Assert.False(admission.IsAdmitted);
        }
        finally
        {
            proxy.Release.TrySetResult();
        }
        using var ready = new MeshReadyBatch();
        while (true)
        {
            ready.Reset();
            owner.DrainReady(MeshReadyDomains.Application, ready, RecvFlags.DontWait);
            if (ready.Count != 0)
                break;
            await Task.Delay(1, bound.Token);
        }
        Assert.Equal(1, ready.Count);
        using var claim = ready.TakeClaim(0);
        using var records = new MeshReceiveBatch();
        Assert.True(claim.Receive(records, RecvFlags.DontWait));
        Assert.Equal(1, records.Count);
        var received = records[0];
        Assert.Equal(MeshRecordKind.NodeRequest, received.Kind);
        using var reply = Message.From("delayed-reply");
        Assert.Equal(SubmitResult.Ok, received.Reply([reply]));
        using var result = await request.WaitAsync(bound.Token);
        Assert.Equal(1, result.PartCount);
        Assert.Equal(
            "delayed-reply",
            System.Text.Encoding.UTF8.GetString(result.ApplicationPayloadView!.Value.GetSpan(0))
        );
        Assert.True(admission.IsAdmitted);
        var connected = requester.ObservePeerRoute(target);
        var retained = Assert.Single(connected.Attempts!, row => row.Endpoint == proxy.Endpoint);
        Assert.Equal(connecting.Attempt.Value.IntentId, retained.IntentId);
        Assert.Equal(ConnectionAttemptState.Connected, retained.State);
        Assert.Equal(0UL, retained.LastFailureId);
        Assert.NotEqual(0UL, connected.ReadyGeneration);
        ready.Reset();
        owner.DrainReady(MeshReadyDomains.Application, ready, RecvFlags.DontWait);
        Assert.Equal(0, ready.Count);
    }

    private sealed class HandshakeGateProxy : IAsyncDisposable
    {
        private readonly TcpListener _listener = new(IPAddress.Loopback, 0);
        private readonly CancellationTokenSource _stop = new();
        private readonly List<Task> _connections = [];
        private Task? _accept;
        internal TaskCompletionSource Accepted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        internal HandshakeGateProxy() => _listener.Start();

        internal string Endpoint => $"tcp://127.0.0.1:{((IPEndPoint)_listener.LocalEndpoint).Port}";

        internal void ForwardTo(string endpoint) => _accept = AcceptAsync(new Uri(endpoint));

        private async Task AcceptAsync(Uri target)
        {
            try
            {
                while (true)
                {
                    var client = await _listener.AcceptTcpClientAsync(_stop.Token);
                    _connections.Add(ForwardAsync(client, target));
                    Accepted.TrySetResult();
                }
            }
            catch (OperationCanceledException) when (_stop.IsCancellationRequested) { }
        }

        private async Task ForwardAsync(TcpClient client, Uri target)
        {
            using (client)
            using (var upstream = new TcpClient())
            {
                try
                {
                    await Release.Task.WaitAsync(_stop.Token);
                    await upstream.ConnectAsync(target.Host, target.Port, _stop.Token);
                    await Task.WhenAll(
                        client.GetStream().CopyToAsync(upstream.GetStream(), _stop.Token),
                        upstream.GetStream().CopyToAsync(client.GetStream(), _stop.Token)
                    );
                }
                catch (OperationCanceledException) when (_stop.IsCancellationRequested) { }
                catch (IOException) when (_stop.IsCancellationRequested) { }
            }
        }

        public async ValueTask DisposeAsync()
        {
            await _stop.CancelAsync();
            if (_accept is not null)
                await _accept;
            _listener.Stop();
            await Task.WhenAll(_connections);
            _stop.Dispose();
        }
    }
}
