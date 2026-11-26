using System.Diagnostics;
using System.Net.Sockets;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime;
using Systems.Zlink.Stream.Connector.Runtime.Protocol;
using Systems.Zlink.Stream.Connector.Runtime.Protocol.Framing;
using Systems.Zlink.Stream.Connector.Runtime.Transport;
using Xunit.Abstractions;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Tests.Common;

namespace Zlink.Framework.UnitTests;

public sealed class StreamPermitLivenessTests(ITestOutputHelper output)
{
    private static readonly TimeSpan Interval = TimeSpan.FromMilliseconds(200);
    private static readonly TimeSpan Heartbeat = TimeSpan.FromSeconds(1);
    private static readonly TimeSpan Idle = TimeSpan.FromMilliseconds(450);
    private const string NodeName = "permit-liveness";

    public static IEnumerable<object[]> Cases()
    {
        var transports = new List<string> { "tcp", "ws", "inproc" };
        if (Systems.Zlink.Zlink.Has("tls"))
            transports.AddRange(["tls", "wss"]);
        if (Socket.OSSupportsUnixDomainSockets && Systems.Zlink.Zlink.Has("ipc"))
            transports.Add("ipc");

        foreach (var transport in transports)
        {
            yield return [transport, "unsaturated", 0];
            yield return [transport, "below_timeout", 300];
            yield return [transport, "at_timeout", 1000];
            yield return [transport, "above_timeout", 1500];
            yield return [transport, "silent_after_release", 1500];
            yield return [transport, "idle_after_release", 1500];
        }
    }

    [Theory]
    [MemberData(nameof(Cases))]
    public async Task Reader_permit_wait_suspends_receive_deadlines(
        string transport,
        string scenario,
        int holdMilliseconds
    )
    {
        using var stop = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        var token = stop.Token;
        var idle = scenario == "idle_after_release";
        var silent = scenario == "silent_after_release";
        var identity = Guid.NewGuid().ToString("N");
        var bind = transport switch
        {
            "inproc" => $"inproc://{identity}",
            "ipc" => $"ipc://{Path.Combine(Path.GetTempPath(), $"zlink-{identity}.sock")}",
            _ => $"{transport}://127.0.0.1:0",
        };
        var inproc = transport == "inproc" ? new InprocBackend() : null;
        var builder = Host.CreateApplicationBuilder();
        builder.Logging.SetMinimumLevel(LogLevel.Warning);
        var connected = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        builder.Services.AddSingleton(connected);
        if (inproc is not null)
            builder.Services.AddSingleton<IZLinkBackendAdapterFactory>(inproc);
        var logDirectory = Environment.GetEnvironmentVariable("ZLINK_STREAM_PERMIT_TEST_LOG_DIR");
        using var flow = logDirectory is null
            ? null
            : new TestHostMessageFlowListener(
                Path.Combine(logDirectory, $"{transport}-{scenario}.flow")
            );
        builder.Services.AddZLinkFramework(options =>
        {
            options.DisableImplicitHandlerAutoRegistration();
            options.ConfigureInboundDispatch().MaxQueuedApplicationJobs = 1;
            if (flow is not null)
                options.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            var stream = options
                .AddStreamNode(NodeName)
                .Bind(bind)
                .SetHeartbeat(Interval, Heartbeat)
                .SetIdleTimeout(idle ? Idle : TimeSpan.Zero)
                .AddSession<ProbeSession>();
            if (transport is "tls" or "wss")
            {
                var certs = Path.Combine(
                    FrameworkTestEnvironment.GetRepoRoot(),
                    "bindings",
                    "dotnet",
                    "tests",
                    "certs"
                );
                stream.SetTlsServer(
                    Path.Combine(certs, "server.crt"),
                    Path.Combine(certs, "server.key")
                );
            }
        });

        using var host = builder.Build();
        await host.StartAsync(token);
        try
        {
            var runtime = host.Services.GetRequiredService<ZLinkFrameworkRuntime>();
            var state = await runtime.EnsureStartedStateAsync(token);
            var endpoint = new Uri(
                runtime.GetListenerStatus(ZLinkListenerKind.Stream, NodeName).Endpoint
            );
            await using var peer = await ConnectAsync(endpoint, inproc, token);
            await connected.Task.WaitAsync(token);
            await peer.PingAsync();
            await peer.ExpectPongAsync();
            if (holdMilliseconds == 0)
            {
                var observation = Stopwatch.StartNew();
                var pongs = 0;
                while (observation.Elapsed < Heartbeat)
                {
                    await peer.PingAsync();
                    await peer.ExpectPongAsync();
                    pongs++;
                    await Task.Delay(40, token);
                }
                Assert.True(pongs > 1);
                output.WriteLine($"{transport} {scenario} pongs={pongs}");
                return;
            }

            // Keep the original receive deadline distinct from the exact-duration hold.
            await Task.Delay(100, token);
            await using var other = silent ? await ConnectAsync(endpoint, inproc, token) : null;
            var probe = other ?? peer;
            long released;
            var jobs = state.ApplicationJobQueue;
            var lease = await jobs.AcquireAsync(token, ZLinkApplicationJobOrigin.Remote);
            try
            {
                await probe.PingAsync();
                while (jobs.GetStatus().CapacityWaiters == 0)
                    await Task.Delay(2, token);

                var held = Stopwatch.StartNew();
                var hold = TimeSpan.FromMilliseconds(holdMilliseconds);
                var pings = 0;
                while (hold - held.Elapsed is var remaining && remaining > TimeSpan.Zero)
                {
                    var frame = await peer.ReadAsync(remaining);
                    if (frame is null)
                        continue;
                    Assert.True(
                        frame.Value.Name == ZlinkStreamControlProtocol.HeartbeatPingName,
                        $"{transport} {scenario} received {frame.Value.Name} ({frame.Value.Reason}) during permit wait after {held.Elapsed.TotalMilliseconds:F0} ms."
                    );
                    pings++;
                }

                output.WriteLine(
                    $"{transport} {scenario} hold_ms={held.Elapsed.TotalMilliseconds:F0} pings={pings}"
                );
                Assert.True(
                    pings >= Math.Max(1, holdMilliseconds / (int)Interval.TotalMilliseconds - 1)
                );
            }
            finally
            {
                released = Stopwatch.GetTimestamp();
                lease.Dispose();
            }

            // A queued packet from another peer can wake the shared reader without
            // refreshing this session's inbound clock.
            await probe.ExpectPongAsync();
            if (!silent && !idle)
                return;

            if (idle)
            {
                await Task.Delay(TimeSpan.FromMilliseconds(350), token);
                await peer.PingAsync();
                await peer.ExpectPongAsync();
            }

            var timeout = idle ? Idle : Heartbeat;
            var observationLimit = timeout + Interval + TimeSpan.FromMilliseconds(100);
            ZlinkStreamCloseReason? reason = null;
            while (
                observationLimit - Stopwatch.GetElapsedTime(released) is var remaining
                && remaining > TimeSpan.Zero
            )
            {
                reason = (await peer.ReadAsync(remaining))?.Reason;
                if (reason is not null)
                    break;
            }
            var elapsed = Stopwatch.GetElapsedTime(released);
            output.WriteLine(
                $"{transport} {scenario} release_to_close_ms={elapsed.TotalMilliseconds:F0} reason={reason}"
            );
            Assert.Equal(
                idle ? ZlinkStreamCloseReason.IdleTimeout : ZlinkStreamCloseReason.HeartbeatTimeout,
                reason
            );
            Assert.True(elapsed >= timeout, $"Session closed after {elapsed}, before {timeout}.");
        }
        finally
        {
            await host.StopAsync();
        }
    }

    private static async ValueTask<Peer> ConnectAsync(
        Uri endpoint,
        InprocBackend? inproc,
        CancellationToken token
    )
    {
        if (inproc is not null)
        {
            var socket = inproc.Context.CreatePairSocket();
            try
            {
                socket.Options.Linger = TimeSpan.Zero;
                socket.Connect(endpoint.OriginalString);
                return new Peer(new PairConnection(socket), token);
            }
            catch
            {
                socket.Dispose();
                throw;
            }
        }

        if (endpoint.Scheme == "ipc")
        {
            var socket = new Socket(
                AddressFamily.Unix,
                System.Net.Sockets.SocketType.Stream,
                ProtocolType.Unspecified
            );
            try
            {
                await socket.ConnectAsync(new UnixDomainSocketEndPoint(endpoint.LocalPath), token);
                return new Peer(
                    new UnixConnection(new NetworkStream(socket, ownsSocket: true)),
                    token
                );
            }
            catch
            {
                socket.Dispose();
                throw;
            }
        }

        return new Peer(
            await ZlinkStreamTransportFactory.ConnectAsync(
                new ZlinkStreamConnectorOptions
                {
                    Endpoint = endpoint,
                    SkipServerCertificateValidation = true,
                },
                token
            ),
            token
        );
    }

    private sealed class Peer(IZlinkStreamConnection connection, CancellationToken token)
        : IAsyncDisposable
    {
        private static readonly byte[] Ping = ZlinkStreamFrameCodec.Encode(
            ZLinkStreamProtocolDefaults
                .EncodeHeader(
                    new ZlinkStreamHeader(
                        ZlinkStreamMessageKind.Control,
                        ZlinkStreamCodec.Raw,
                        ZlinkStreamHeaderFlags.None,
                        null,
                        ZlinkStreamControlProtocol.HeartbeatPingName,
                        ZlinkStreamMetadata.Empty
                    )
                )
                .Span,
            []
        );

        private readonly CancellationTokenSource _reading =
            CancellationTokenSource.CreateLinkedTokenSource(token);
        private Task<ZlinkStreamFrame>? _pending;

        public ValueTask PingAsync() => connection.WriteAsync(Ping, token);

        public async Task ExpectPongAsync()
        {
            (string Name, ZlinkStreamCloseReason? Reason) frame;
            do
            {
                frame =
                    await ReadAsync(TimeSpan.FromSeconds(2))
                    ?? throw new TimeoutException(
                        "No STREAM control frame while waiting for pong."
                    );
            } while (frame.Name == ZlinkStreamControlProtocol.HeartbeatPingName);
            Assert.Equal(ZlinkStreamControlProtocol.HeartbeatPongName, frame.Name);
        }

        public async Task<(string Name, ZlinkStreamCloseReason? Reason)?> ReadAsync(
            TimeSpan duration
        )
        {
            _pending ??= ZlinkStreamFrameCodec.ReadAsync(connection, 4096, _reading.Token).AsTask();
            ZlinkStreamFrame frame;
            try
            {
                frame = await _pending.WaitAsync(duration);
            }
            catch (TimeoutException)
            {
                // Keep the pending read across observation windows even if a frame is partial.
                return null;
            }

            _pending = null;
            var header = ZLinkStreamProtocolDefaults.DecodeHeader(frame.Header);
            return (
                header.Name,
                header.Name == ZlinkStreamSessionClosingCodec.ControlName
                    ? ZlinkStreamSessionClosingCodec.Decode(frame.Payload.Span).Reason
                    : null
            );
        }

        public async ValueTask DisposeAsync()
        {
            await _reading.CancelAsync();
            try
            {
                if (_pending is not null)
                    await _pending;
            }
            catch (OperationCanceledException) when (_reading.IsCancellationRequested) { }
            finally
            {
                _reading.Dispose();
                await connection.CloseAsync(CancellationToken.None);
            }
        }
    }

    private sealed class UnixConnection(NetworkStream stream) : IZlinkStreamConnection
    {
        public ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken token) =>
            stream.ReadAsync(buffer, token);

        public ValueTask WriteAsync(ReadOnlyMemory<byte> buffer, CancellationToken token) =>
            stream.WriteAsync(buffer, token);

        public ValueTask CloseAsync(CancellationToken token) => stream.DisposeAsync();
    }

    private sealed class PairConnection(IPairSocket socket) : IZlinkStreamConnection
    {
        private ReadOnlyMemory<byte> _remaining;

        public async ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken token)
        {
            if (_remaining.IsEmpty)
            {
                using var received = Received.Create();
                while (!socket.Recv(received, RecvFlags.DontWait))
                    await Task.Delay(2, token);
                _remaining = Assert.Single(received.Parts).AsReadOnlySpan().ToArray();
            }
            var count = Math.Min(buffer.Length, _remaining.Length);
            _remaining[..count].CopyTo(buffer);
            _remaining = _remaining[count..];
            return count;
        }

        public ValueTask WriteAsync(ReadOnlyMemory<byte> buffer, CancellationToken token)
        {
            using var message = Message.From(buffer.Span);
            socket.Send().Message(message).Submit();
            return ValueTask.CompletedTask;
        }

        public ValueTask CloseAsync(CancellationToken token) => socket.DisposeAsync();
    }

    private sealed class InprocBackend : IZLinkBackendAdapterFactory
    {
        private readonly ZLinkDotNetBackendAdapterFactory _backend = new();

        public IContext Context { get; } = Systems.Zlink.Zlink.CreateContext();

        public IZLinkBackendRuntimeContext CreateRuntimeContext() =>
            new ZLinkDotNetBackendRuntimeContext(Context);

        public IZLinkMonitoringBackendAdapter CreateMonitoringAdapter() =>
            _backend.CreateMonitoringAdapter();
    }

    private sealed class ProbeSession(IZLinkSessionContext context, TaskCompletionSource connected)
        : IZLinkSession
    {
        public IZLinkSessionContext Context => context;

        public ValueTask OnConnectedAsync(CancellationToken token)
        {
            connected.TrySetResult();
            return ValueTask.CompletedTask;
        }

        public ValueTask OnDisconnectedAsync(CancellationToken token) => ValueTask.CompletedTask;

        public ValueTask OnDispatchAsync(
            ZLinkSessionDispatchContext dispatch,
            ZLinkMessage payload,
            CancellationToken token
        ) => ValueTask.CompletedTask;

        public ValueTask OnErrorAsync(ZLinkStreamError error, CancellationToken token) =>
            ValueTask.CompletedTask;
    }
}
