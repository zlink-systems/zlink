using System.Buffers.Binary;
using System.Net.Sockets;
using System.Text.Json;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime.Protocol;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed class AdmissionQueueMeshTests
{
    [Fact]
    public async Task TwoNodesAndThreeSessionsRespectTheGlobalLimit()
    {
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(25));
        var token = deadline.Token;
        using var flow = Environment.GetEnvironmentVariable("ZLINK_ADMISSION_QUEUE_TEST_FLOW")
            is { } path
            ? new TestHostMessageFlowListener(Path.Combine(path, "admission-mesh.flow"))
            : null;
        var location = new ZLinkInMemoryProviderLocationStore();
        var relocation = new InMemoryRelocationStore();
        var probe = new HandlerProbe();
        using var server = CreateHost(location, relocation, probe, isServer: true);
        using var client = CreateHost(location, relocation, probe, isServer: false);
        await server.StartAsync(token);
        await client.StartAsync(token);
        try
        {
            using var first = await Peer.ConnectAsync(server, token);
            using var second = await Peer.ConnectAsync(client, token);
            using var third = await Peer.ConnectAsync(client, token);
            var firstAdmission = first.AuthenticateAsync(token);
            if (
                await Task.WhenAny(firstAdmission, probe.FirstEntered.Task).WaitAsync(token)
                == firstAdmission
            )
                await firstAdmission;
            await probe.FirstEntered.Task.WaitAsync(token);

            var secondAdmission = second.AuthenticateAsync(token);
            var thirdAdmission = third.AuthenticateAsync(token);
            var queued = Task.WhenAll(second.Queued.Task, third.Queued.Task);
            var queuedOrComplete = await Task.WhenAny(queued, secondAdmission, thirdAdmission)
                .WaitAsync(token);
            await queuedOrComplete;
            Assert.Same(queued, queuedOrComplete);
            var pushOrComplete = await Task.WhenAny(
                    second.PositionPushed.Task,
                    secondAdmission,
                    thirdAdmission
                )
                .WaitAsync(token);
            await pushOrComplete;
            Assert.Same(second.PositionPushed.Task, pushOrComplete);
            Assert.Equal(1, Volatile.Read(ref probe.Entered));
            Assert.Equal(1, Volatile.Read(ref probe.Active));

            probe.ReleaseFirst.TrySetResult();
            await Task.WhenAll(firstAdmission, secondAdmission, thirdAdmission).WaitAsync(token);
            Assert.Equal(3, Volatile.Read(ref probe.Entered));
            Assert.Equal(1, Volatile.Read(ref probe.Peak));
        }
        finally
        {
            probe.ReleaseFirst.TrySetResult();
            await client.StopAsync(CancellationToken.None);
            await server.StopAsync(CancellationToken.None);
        }
    }

    internal static IHost CreateHost(
        ZLinkInMemoryProviderLocationStore location,
        InMemoryRelocationStore relocation,
        HandlerProbe probe,
        bool isServer,
        int? globalLimit = 1,
        IZLinkSpotClient? spotClient = null
    )
    {
        var path = Environment.GetEnvironmentVariable("ZLINK_ADMISSION_QUEUE_TEST_FLOW");
        var host = Host.CreateDefaultBuilder();
        return host.ConfigureServices(services =>
            {
                services.AddSingleton(probe);
                services.AddZLinkFramework(options =>
                {
                    options.DisableImplicitHandlerAutoRegistration();
                    if (path is not null)
                        options
                            .ConfigureDispatch()
                            .Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
                    options.AddLocationStore(location);
                    options.AddRelocationStore(relocation);
                    var roles = options
                        .AddRouteMesh("admission")
                        .Listen("tcp://127.0.0.1:0")
                        .Objects();
                    if (isServer)
                        roles.Server();
                    else
                        roles.Client();
                    var listener = options.AddStreamNode("login");
                    listener
                        .Bind("tcp://127.0.0.1:0")
                        .SetHeartbeat(TimeSpan.FromSeconds(30), TimeSpan.FromSeconds(65))
                        .AddSession<LoginSession>();
                    listener.AuthenticationPacketName = "Login";
                    listener.MaxConcurrentAuthentications = 4;
                    listener.AdmissionQueueEnabled = true;
                    listener.AdmissionQueueKey = AdmissionQueueFixture.Key;
                    listener.AdmissionGlobalLimit = globalLimit;
                });
                if (spotClient is not null)
                    services.AddSingleton(spotClient);
            })
            .Build();
    }

    internal sealed class HandlerProbe
    {
        internal readonly TaskCompletionSource FirstEntered = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        internal readonly TaskCompletionSource ReleaseFirst = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        internal readonly TaskCompletionSource<RoutingId> Connected = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        internal bool FailHandler { get; init; }
        internal int Active;
        internal int Peak;
        internal int Entered;

        internal void RecordPeak(int current)
        {
            int previous;
            do
            {
                previous = Volatile.Read(ref Peak);
                if (previous >= current)
                    return;
            } while (Interlocked.CompareExchange(ref Peak, current, previous) != previous);
        }
    }

    private sealed class LoginSession(IZLinkSessionContext context, HandlerProbe probe)
        : IZLinkSession
    {
        public IZLinkSessionContext Context { get; } = context;

        public ValueTask OnConnectedAsync(CancellationToken cancellationToken)
        {
            probe.Connected.TrySetResult(Context.RoutingId!.Value);
            return ValueTask.CompletedTask;
        }

        public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;

        public ValueTask OnErrorAsync(
            ZLinkStreamError error,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;

        public async ValueTask OnDispatchAsync(
            ZLinkSessionDispatchContext dispatch,
            ZLinkMessage request,
            CancellationToken cancellationToken
        )
        {
            var current = Interlocked.Increment(ref probe.Active);
            probe.RecordPeak(current);
            try
            {
                if (Interlocked.Increment(ref probe.Entered) == 1)
                {
                    probe.FirstEntered.TrySetResult();
                    await probe.ReleaseFirst.Task.WaitAsync(cancellationToken);
                }
                if (probe.FailHandler)
                    throw new InvalidOperationException("Authentication handler failed.");
                await Context.Client.Reply(new LoggedIn(true)).Async();
            }
            finally
            {
                Interlocked.Decrement(ref probe.Active);
            }
        }
    }

    private sealed record LoggedIn(bool Accepted);

    internal sealed class Peer(TcpClient socket) : IDisposable
    {
        private ulong _seq;
        internal TaskCompletionSource PositionPushed { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource Queued { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        internal static async Task<Peer> ConnectAsync(
            IHost host,
            CancellationToken cancellationToken
        )
        {
            var endpoint = new Uri(
                host.Services.GetRequiredService<IZLinkFrameworkRuntime>()
                    .GetListenerStatus(ZLinkListenerKind.Stream, "login")
                    .Endpoint
            );
            var socket = new TcpClient();
            try
            {
                await socket.ConnectAsync(endpoint.Host, endpoint.Port, cancellationToken);
                return new Peer(socket);
            }
            catch
            {
                socket.Dispose();
                throw;
            }
        }

        internal async Task AuthenticateAsync(CancellationToken cancellationToken)
        {
            string? ticket = null;
            while (true)
            {
                var metadata = ticket is null
                    ? ZlinkStreamMetadata.Empty
                    : ZlinkStreamMetadata.Empty.With("$zlink.admission.ticket", ticket);
                await SendRequestAsync("Login", metadata, cancellationToken);
                var (reply, payload) = await ReadAsync(cancellationToken);
                using (payload)
                {
                    if (ticket is not null && reply.Kind == ZlinkStreamMessageKind.Response)
                    {
                        Assert.True(payload.RootElement.GetProperty("accepted").GetBoolean());
                        return;
                    }
                    Assert.Equal(ZlinkStreamMessageKind.Error, reply.Kind);
                    Assert.Equal(
                        "unavailable",
                        payload.RootElement.GetProperty("code").GetString()
                    );
                    Assert.Equal("1", reply.Metadata.Get("$zlink.admission.queueRequired"));
                }
                await SendRequestAsync(
                    "$zlink.admission.enqueue",
                    ZlinkStreamMetadata.Empty,
                    cancellationToken
                );
                ticket = null;
                while (ticket is null)
                {
                    var (delivery, body) = await ReadAsync(cancellationToken);
                    using (body)
                    {
                        Assert.True(
                            delivery.Kind
                                is ZlinkStreamMessageKind.Response
                                    or ZlinkStreamMessageKind.Send,
                            body.RootElement.GetRawText()
                        );
                        if (body.RootElement.TryGetProperty("ticket", out var value))
                            ticket = value.GetString();
                        else
                        {
                            Assert.True(body.RootElement.GetProperty("position").GetInt32() > 0);
                            Queued.TrySetResult();
                            if (delivery.Kind == ZlinkStreamMessageKind.Send)
                                PositionPushed.TrySetResult();
                        }
                    }
                }
            }
        }

        internal async Task SendRequestAsync(
            string name,
            ZlinkStreamMetadata metadata,
            CancellationToken cancellationToken
        )
        {
            var header =
                name == "$zlink.admission.enqueue"
                    ? EncodeEnqueueHeader(++_seq)
                    : ZLinkStreamProtocolDefaults.EncodeHeader(
                        new ZlinkStreamHeader(
                            ZlinkStreamMessageKind.Request,
                            ZlinkStreamCodec.Json,
                            ZlinkStreamHeaderFlags.HasRequestSeq,
                            new ZlinkStreamRequestSeq(++_seq),
                            name,
                            metadata
                        )
                    );
            var bytes = ZLinkStreamFrameCodec.Encode(header.Span, "{}"u8);
            await socket.GetStream().WriteAsync(bytes, cancellationToken);
        }

        private static ReadOnlyMemory<byte> EncodeEnqueueHeader(ulong sequence)
        {
            // Emit the specified peer wire bytes independently of the local
            // connector's reserved packet-name support.
            ReadOnlySpan<byte> name = "$zlink.admission.enqueue"u8;
            var header = new byte[13 + name.Length];
            header[0] = ZlinkStreamFlowId.FormatMarker;
            header[1] = (byte)ZlinkStreamMessageKind.Request;
            header[2] = (byte)ZlinkStreamCodec.Json;
            header[3] = (byte)ZlinkStreamHeaderFlags.HasRequestSeq;
            BinaryPrimitives.WriteUInt64BigEndian(header.AsSpan(4), sequence);
            header[12] = (byte)name.Length;
            name.CopyTo(header.AsSpan(13));
            return header;
        }

        internal async Task<(ZlinkStreamHeader Header, JsonDocument Payload)> ReadAsync(
            CancellationToken cancellationToken
        )
        {
            var network = socket.GetStream();
            var prefix = new byte[6];
            await network.ReadExactlyAsync(prefix, cancellationToken);
            var headerLength = BinaryPrimitives.ReadUInt16BigEndian(prefix);
            var payloadLength = checked(
                (int)BinaryPrimitives.ReadUInt32BigEndian(prefix.AsSpan(2))
            );
            var bytes = new byte[checked(headerLength + payloadLength)];
            await network.ReadExactlyAsync(bytes, cancellationToken);
            var header = ZLinkStreamProtocolDefaults.DecodeHeader(bytes.AsMemory(0, headerLength));
            Assert.True(
                header.Codec == ZlinkStreamCodec.Json,
                $"Expected an admission JSON packet. Received {header.Kind} '{header.Name}' with codec {header.Codec}."
            );
            return (header, JsonDocument.Parse(bytes.AsMemory(headerLength)));
        }

        public void Dispose() => socket.Dispose();
    }
}
