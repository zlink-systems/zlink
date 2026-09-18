using System.Net;
using System.Net.Sockets;
using Systems.Zlink.Stream.Connector.Contracts;
using Xunit;

public sealed partial class StreamConnectorTests
{
    /// <summary>
    ///     A wait predicate is caller code — on the typed surface it decodes the payload
    ///     first — so it must not run while the receive queue lock is held, and it must be
    ///     free to read the lifecycle surfaces the connector exposes (spec §10).
    /// </summary>
    [Fact]
    public async Task WaitPredicateRunsOutsideTheReceiveQueueLockAndMayReadConnectorState()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var headerCodec = new ZlinkStreamHeaderCodec();
        var done = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            await WritePacketAsync(
                stream,
                headerCodec.Encode(new ZlinkStreamHeader(
                    ZlinkStreamMessageKind.Send,
                    ZlinkStreamCodec.Raw,
                    ZlinkStreamHeaderFlags.None,
                    null,
                    "slow-predicate",
                    ZlinkStreamMetadata.Empty)).ToArray(),
                "payload"u8.ToArray());
            await done.Task.WaitAsync(TimeSpan.FromSeconds(15));
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat(),
            Reconnect = new ZlinkStreamReconnectOptions { Enabled = false }
        });

        await connector.Connect.Async().AsTask().WaitAsync(TimeSpan.FromSeconds(15));
        await WaitUntilAsync(
            () => connector.ReceivedCount("slow-predicate") == 1,
            TimeSpan.FromSeconds(15));

        var predicateEntered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var releasePredicate = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var stateInsidePredicate = ZlinkStreamConnectionState.Created;
        var wait = Task.Run(async () => await connector
            .WaitFor("slow-predicate")
            .Timeout(TimeSpan.FromSeconds(15))
            .Where(_ =>
            {
                // Reading the lifecycle surface takes the lifecycle lock. Doing it from a
                // predicate that still held the receive queue lock would be the reverse of
                // the order the reconnect path takes the two.
                stateInsidePredicate = connector.State;
                predicateEntered.TrySetResult();
                releasePredicate.Task.Wait(TimeSpan.FromSeconds(15));
                return true;
            })
            .Async());

        await predicateEntered.Task.WaitAsync(TimeSpan.FromSeconds(15));

        // ReceivedCount takes the receive queue lock the receive thread takes for every
        // arrival. It answers while the predicate is still running, so the predicate does
        // not hold that lock.
        var counted = await Task.Run(() => connector.ReceivedCount("slow-predicate"))
            .WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(1, counted);

        releasePredicate.SetResult();
        var message = await wait.WaitAsync(TimeSpan.FromSeconds(15));

        Assert.Equal("slow-predicate", message.Name);
        Assert.Equal(ZlinkStreamConnectionState.Connected, stateInsidePredicate);

        done.SetResult();
        await server.WaitAsync(TimeSpan.FromSeconds(15));
    }

    /// <summary>
    ///     A connection that is established drops what the previous connection left
    ///     unconsumed, so a wait on the new connection never takes a packet from before the
    ///     drop (spec §10).
    /// </summary>
    [Fact]
    public async Task ConnectionEstablishedDropsThePreviousConnectionsUnconsumedMessages()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var headerCodec = new ZlinkStreamHeaderCodec();
        var staleRecorded = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var reconnected = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var done = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var server = Task.Run(async () =>
        {
            using (var first = await listener.AcceptTcpClientAsync())
            {
                await using var stream = first.GetStream();
                await WritePacketAsync(
                    stream,
                    headerCodec.Encode(new ZlinkStreamHeader(
                        ZlinkStreamMessageKind.Send,
                        ZlinkStreamCodec.Raw,
                        ZlinkStreamHeaderFlags.None,
                        null,
                        "stale",
                        ZlinkStreamMetadata.Empty)).ToArray(),
                    "stale-payload"u8.ToArray());
                await staleRecorded.Task.WaitAsync(TimeSpan.FromSeconds(15));
            }

            using var second = await listener.AcceptTcpClientAsync();
            await using var secondStream = second.GetStream();
            reconnected.TrySetResult();
            await done.Task.WaitAsync(TimeSpan.FromSeconds(15));
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat(),
            Reconnect = new ZlinkStreamReconnectOptions
            {
                InitialDelay = TimeSpan.FromMilliseconds(10),
                MaxDelay = TimeSpan.FromMilliseconds(10),
                BackoffFactor = 1.0,
                MaxAttempts = 20
            }
        });

        await connector.Connect.Async().AsTask().WaitAsync(TimeSpan.FromSeconds(15));
        await WaitUntilAsync(
            () => connector.ReceivedCount("stale") == 1,
            TimeSpan.FromSeconds(15));
        staleRecorded.SetResult();

        await reconnected.Task.WaitAsync(TimeSpan.FromSeconds(15));
        await WaitUntilAsync(
            () => connector.State == ZlinkStreamConnectionState.Connected
                  && connector.ReceivedCount("stale") == 0,
            TimeSpan.FromSeconds(15));

        // The counters say the new connection received nothing, and the history has to say
        // the same thing: the wait finds nothing and ends on its own timeout.
        var failure = await Assert.ThrowsAsync<ZlinkStreamException>(async () =>
            await connector.WaitFor("stale").Timeout(TimeSpan.FromMilliseconds(200)).Async());

        Assert.Equal(ZlinkStreamErrorCode.ValidationFailed, failure.Error.Code);

        done.SetResult();
        await server.WaitAsync(TimeSpan.FromSeconds(15));
    }

    /// <summary>
    ///     A wait that was still pending when the connection it observed ended has no place
    ///     left to observe, which is <c>Disconnected</c> rather than a violated observation
    ///     (spec §10.1).
    /// </summary>
    [Fact]
    public async Task WaitPendingWhenTheNextConnectionIsEstablishedFailsAsDisconnected()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var dropFirst = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var reconnected = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var done = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var server = Task.Run(async () =>
        {
            using (var first = await listener.AcceptTcpClientAsync())
            {
                await dropFirst.Task.WaitAsync(TimeSpan.FromSeconds(15));
            }

            using var second = await listener.AcceptTcpClientAsync();
            await using var secondStream = second.GetStream();
            reconnected.TrySetResult();
            await done.Task.WaitAsync(TimeSpan.FromSeconds(15));
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat(),
            Reconnect = new ZlinkStreamReconnectOptions
            {
                InitialDelay = TimeSpan.FromMilliseconds(10),
                MaxDelay = TimeSpan.FromMilliseconds(10),
                BackoffFactor = 1.0,
                MaxAttempts = 20
            }
        });

        await connector.Connect.Async().AsTask().WaitAsync(TimeSpan.FromSeconds(15));
        var wait = connector.WaitFor("never-arrives").Timeout(TimeSpan.FromSeconds(15)).Async().AsTask();
        dropFirst.SetResult();

        await reconnected.Task.WaitAsync(TimeSpan.FromSeconds(15));
        var failure = await Assert.ThrowsAsync<ZlinkStreamException>(async () =>
            await wait.WaitAsync(TimeSpan.FromSeconds(15)));

        Assert.Equal(ZlinkStreamErrorCode.Disconnected, failure.Error.Code);

        done.SetResult();
        await server.WaitAsync(TimeSpan.FromSeconds(15));
    }
}
