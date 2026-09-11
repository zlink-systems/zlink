using System.Buffers.Binary;
using System.Net;
using System.Net.Sockets;
using System.Text.Json;
using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Stream.Connector.Contracts;
using Xunit.Abstractions;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Codecs;

namespace Zlink.Framework.UnitTests;

public sealed class StreamBackpressureTests(ITestOutputHelper output)
{
    [Theory]
    [InlineData(SendFlags.None)]
    [InlineData(SendFlags.DontWait)]
    public async Task StreamWrite_RetainsBackpressuredPayloadWithoutTerminalRefusal(SendFlags flags)
    {
        await using var fixture = await SaturatedStream.CreateAsync(output);
        var saturation = await fixture.SaturateAsync();
        IZLinkStream stream = fixture.Stream;
        var expected = Enumerable.Repeat((byte)91, 2048).ToArray();

        Assert.True(stream.Write(ZLinkMessage.From(expected), flags));
        var received = await fixture.ReadSubmissionAsync(
            saturation, JsonSerializer.SerializeToUtf8Bytes(expected).Length);
        Assert.Equal(expected, JsonSerializer.Deserialize<byte[]>(received));
        await fixture.AssertMarkerAsync();
        output.WriteLine($"public Write({flags}) returned true; payload delivered once in order");
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task SessionTypedSendAndReply_WaitForAdmissionWithoutTerminalRefusal(bool reply)
    {
        await using var fixture = await SaturatedStream.CreateAsync(output);
        using var runtime = new RuntimeFixture();
        var context = new ZLinkSessionContext(
            runtime.Runtime,
            fixture.Stream,
            new TestSessionHandlerRegistry(),
            static () => ValueTask.CompletedTask,
            static _ => ValueTask.CompletedTask);
        IZLinkSessionContext session = context;
        var expected = new List<BackpressurePayload>();
        Task? pending = null;
        for (var sequence = 0; sequence < 10000; sequence++)
        {
            if (reply)
                _ = context.EnterDispatch(CreateRequestHeader() with
                {
                    RequestSeq = new ZlinkStreamRequestSeq(checked((uint)sequence + 1))
                });
            var payload = new BackpressurePayload(
                $"{(reply ? "typed-reply" : "typed-send")}-{sequence}:" + new string('x', 2048));
            expected.Add(payload);
            var submission = reply
                ? session.Client.Reply(payload).Async(fixture.Token).AsTask()
                : session.Client.Send(payload).Async(fixture.Token).AsTask();
            await Task.WhenAny(submission, Task.Delay(100, fixture.Token));
            fixture.Token.ThrowIfCancellationRequested();
            if (!submission.IsCompleted)
            {
                pending = submission;
                break;
            }
            await submission;
        }
        Assert.NotNull(pending);
        Assert.False(pending.IsCompleted,
            $"Expected pending admission; status={pending.Status}; error={pending.Exception}");
        output.WriteLine($"public typed {(reply ? "Reply" : "Send")} admission pending after {expected.Count} submissions");
        if (reply)
        {
            await Assert.ThrowsAsync<InvalidOperationException>(() =>
                session.Client.Reply(new BackpressurePayload("duplicate"))
                    .Async(fixture.Token).AsTask());
        }

        for (var index = 0; index < expected.Count; index++)
        {
            var received = DecodeFrame(await fixture.ReadFrameAsync());
            Assert.Equal(reply ? ZlinkStreamMessageKind.Response : ZlinkStreamMessageKind.Send,
                received.Header.Kind);
            Assert.Equal(ZlinkStreamCodec.Json, received.Header.Codec);
            Assert.Equal(expected[index], JsonSerializer.Deserialize<BackpressurePayload>(
                received.Payload, new JsonSerializerOptions { PropertyNameCaseInsensitive = true }));
            if (reply)
                Assert.Equal(new ZlinkStreamRequestSeq(checked((uint)index + 1)), received.Header.RequestSeq);
        }
        await pending.WaitAsync(fixture.Token);
        await fixture.AssertMarkerAsync();
        output.WriteLine("typed JSON frames delivered once in order; admission completed without terminal refusal");
    }

    [Fact]
    public async Task DirectNoBindReply_BackpressuredSubmissionIsNotTerminalRefusal()
    {
        using var runtime = new RuntimeFixture();
        var actor = new ZLinkBackendActorRef(RoutingId.From("actor-node"), "actor", 1);
        var header = CreateRequestHeader();
        var reply = ZLinkActorReply.FromError(new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.Unavailable, "actor-terminal-payload" + new string('x', 2048)));
        var expected = reply.ToFrame(header);
        byte[]? captured = null;
        var attempts = 0;

        await ZLinkActorBoundSessionRelay.SendReplyAsync(
            runtime.Runtime,
            actor.ActorId,
            actor,
            RoutingId.From("source-node"),
            RoutingId.From("source-session"),
            requestId: 19,
            flags: ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
            replyCapability: "reply-capability",
            isNoBind: true,
            requestHeader: header,
            reply: reply,
            cancellationToken: CancellationToken.None,
            directReply: parts =>
            {
                attempts++;
                captured = Assert.Single(parts).AsReadOnlySpan().ToArray();
                return SubmitResult.Backpressured;
            });

        Assert.Equal(expected, captured);
        Assert.Equal(1, attempts);
        output.WriteLine("direct reply accepted Backpressured snapshot and invoked the callback once");
    }

    [Fact]
    public async Task BoundSessionSend_RetainsBackpressuredPayloadWithoutTerminalRefusal()
    {
        await using var fixture = await SaturatedStream.CreateAsync(output);
        await using var node = new ZLinkManagedMeshNode(fixture.Context, "mesh");
        node.SetRoutingId(RoutingId.From("backpressure-session-node"));
        var actor = node.CreateActor("backpressure-actor");
        await using var sessions = node.CreateStreamSessionService(fixture.Socket);
        sessions.Start();
        Assert.Equal(SubmitResult.Ok, sessions.BindActor(
            fixture.RoutingId, actor, out _, TimeSpan.FromSeconds(5)));
        Assert.Equal(actor, Assert.Single(sessions.Bindings(fixture.RoutingId)).Actor);
        var saturation = await fixture.SaturateAsync();
        var expected = Enumerable.Repeat((byte)19, 2048).ToArray();
        using var payload = Message.From(expected);

        Assert.Equal(SubmitResult.Ok, node.SendBoundSession(actor, [payload]));
        Assert.Equal(expected, await fixture.ReadSubmissionAsync(saturation, expected.Length));
        await fixture.AssertMarkerAsync();
        output.WriteLine("managed bound session returned Ok; retained payload delivered once in order");
    }

    private static ReceivedFrame DecodeFrame(byte[] frame)
    {
        Assert.True(ZLinkStreamFrameCodec.TryDecode(frame, out var header, out var payload));
        return new ReceivedFrame(ZLinkStreamProtocolDefaults.DecodeHeader(header.ToArray()), payload.ToArray());
    }

    private static ZlinkStreamHeader CreateRequestHeader() => new(
        ZlinkStreamMessageKind.Request,
        ZlinkStreamCodec.Json,
        ZlinkStreamHeaderFlags.HasRequestSeq,
        new ZlinkStreamRequestSeq(17),
        "backpressure-request",
        ZlinkStreamMetadata.Empty);

    private sealed record BackpressurePayload(string Value);

    private sealed class RuntimeFixture : IDisposable
    {
        private readonly ServiceProvider _provider;

        public RuntimeFixture()
        {
            var registration = new ZLinkFrameworkRegistration();
            _provider = new ServiceCollection().AddSingleton(registration).BuildServiceProvider();
            Runtime = new ZLinkFrameworkRuntime(
                _provider,
                null!,
                registration,
                new ZLinkHandlerRegistry([]),
                new ZLinkHandlerDispatcher(
                    _provider.GetRequiredService<IServiceScopeFactory>(), registration));
        }

        public ZLinkFrameworkRuntime Runtime { get; }

        public void Dispose() => _provider.Dispose();
    }

    private sealed class TestSessionHandlerRegistry : IZLinkSessionHandlerRegistry
    {
        public void AddHandler<THandler>() where THandler : class { }
        public void AddHandler<THandler>(string packetName) where THandler : class { }

        public ValueTask<bool> TryHandleAsync(
            ZLinkSessionDispatchContext dispatch,
            ZLinkMessage payload,
            CancellationToken cancellationToken = default) => ValueTask.FromResult(false);
    }

    private sealed class SaturatedStream : IAsyncDisposable
    {
        private const int FillerSize = 1024;
        private static readonly byte[] Marker = [211, 173, 139, 107, 83, 61, 41, 23];
        private readonly ITestOutputHelper _output;
        private readonly CancellationTokenSource _timeout = new(TimeSpan.FromSeconds(10));
        private readonly TcpClient _peer = new() { ReceiveBufferSize = 4096 };
        private readonly ZLinkBackendStreamSocketWrapper _backend;

        private SaturatedStream(ITestOutputHelper output)
        {
            _output = output;
            Context = Systems.Zlink.Zlink.CreateContext();
            Context.Options.AutoHwmEnabled = false;
            Socket = Context.CreateStreamSocket();
            Socket.Options.ReceiveMode = StreamReceiveMode.Packet;
            Socket.Options.SendHighWaterMark = 4096;
            Socket.Options.SendBufferSize = 4096;
            Socket.Options.ReceiveTimeout = TimeSpan.FromSeconds(5);
            Socket.Options.Linger = TimeSpan.Zero;
            _backend = new ZLinkBackendStreamSocketWrapper(
                Socket, null!, new ZLinkMeshCompletionTable(), ownsNode: false);
        }

        public IContext Context { get; }
        public IStreamSocket Socket { get; }
        public RoutingId RoutingId { get; private set; }
        public ZLinkManagedStream Stream { get; private set; } = null!;
        public CancellationToken Token => _timeout.Token;

        public static async Task<SaturatedStream> CreateAsync(ITestOutputHelper output)
        {
            var fixture = new SaturatedStream(output);
            try
            {
                fixture.Socket.Bind("tcp://127.0.0.1:*");
                await fixture._peer.ConnectAsync(
                    IPAddress.Loopback, new Uri(fixture.Socket.Options.LastEndpoint).Port, fixture.Token);
                await fixture._peer.GetStream().WriteAsync(
                    new byte[] { 0, 0, 0, 0, 0, 1, 42 }, fixture.Token);
                using var packet = StreamPacket.Create();
                Assert.True(fixture.Socket.RecvPacket(packet));
                fixture.RoutingId = packet.RoutingId!.Value;
                fixture.Stream = new ZLinkManagedStream(
                    fixture._backend, fixture.RoutingId, new ZLinkCodecRegistryBuilder(), "tcp");
                return fixture;
            }
            catch
            {
                await fixture.DisposeAsync();
                throw;
            }
        }

        public async Task<Saturation> SaturateAsync()
        {
            for (var sequence = 0; sequence < 10000; sequence++)
            {
                Token.ThrowIfCancellationRequested();
                var bytes = new byte[FillerSize];
                BinaryPrimitives.WriteInt32LittleEndian(bytes, sequence);
                using var message = Message.From(bytes);
                var submission = Socket.Send(RoutingId).Message(message).Async(Token);
                if (submission.Result == SubmitResult.Backpressured)
                {
                    // Let initial I/O catch up before declaring the unread peer saturated.
                    // Each iteration is a distinct sequence, never a resubmission.
                    await Task.WhenAny(submission.Admitted, Task.Delay(100, Token));
                    Token.ThrowIfCancellationRequested();
                    if (!submission.Admitted.IsCompleted)
                    {
                        _output.WriteLine($"snapshot={submission.Result}; admittedCompleted=false; sequence={sequence}");
                        return new Saturation(sequence + 1, submission.Admitted);
                    }
                }
                await submission.Admitted.WaitAsync(Token);
            }
            throw new Xunit.Sdk.XunitException("Did not reach native STREAM backpressure within 10000 messages.");
        }

        public async Task<byte[]> ReadSubmissionAsync(Saturation saturation, int? targetLength = null)
        {
            // These sends were individually admitted before the next submit.
            for (var sequence = 0; sequence < saturation.Count - 1; sequence++)
                AssertFiller(sequence, await ReadAsync(FillerSize));

            // The retained filler and the target are independent pending operations.
            // Admission completion does not order them relative to each other.
            var first = await ReadAsync(4);
            var fillerFirst = BinaryPrimitives.ReadInt32LittleEndian(first) == saturation.Count - 1;
            if (fillerFirst)
            {
                AssertFiller(saturation.Count - 1, await ReadRestAsync(first, FillerSize));
                first = await ReadAsync(4);
            }
            if (targetLength is null)
            {
                first = await ReadRestAsync(first, 6);
                var headerLength = BinaryPrimitives.ReadUInt16BigEndian(first);
                var payloadLength = BinaryPrimitives.ReadUInt32BigEndian(first.AsSpan(2));
                Assert.InRange(payloadLength, 1u, 16384u);
                targetLength = checked(6 + headerLength + (int)payloadLength);
            }
            var target = await ReadRestAsync(first, targetLength.Value);
            if (!fillerFirst)
                AssertFiller(saturation.Count - 1, await ReadAsync(FillerSize));
            await saturation.Admitted.WaitAsync(Token);
            _output.WriteLine($"drained={saturation.Count}; retained filler and target delivered once");
            return target;
        }

        private static void AssertFiller(int sequence, byte[] actual)
        {
            var expected = new byte[FillerSize];
            BinaryPrimitives.WriteInt32LittleEndian(expected, sequence);
            Assert.Equal(expected, actual);
        }

        private async Task<byte[]> ReadRestAsync(byte[] prefix, int length)
        {
            var bytes = new byte[length];
            prefix.CopyTo(bytes, 0);
            await _peer.GetStream().ReadExactlyAsync(bytes.AsMemory(prefix.Length), Token);
            return bytes;
        }

        public async Task<byte[]> ReadAsync(int length)
        {
            var bytes = new byte[length];
            await _peer.GetStream().ReadExactlyAsync(bytes, Token);
            return bytes;
        }

        public async Task<byte[]> ReadFrameAsync()
        {
            var prefix = await ReadAsync(6);
            var headerLength = BinaryPrimitives.ReadUInt16BigEndian(prefix);
            var payloadLength = BinaryPrimitives.ReadUInt32BigEndian(prefix.AsSpan(2));
            Assert.InRange(payloadLength, 1u, 16384u);
            return await ReadRestAsync(prefix, checked(6 + headerLength + (int)payloadLength));
        }

        public async Task AssertMarkerAsync()
        {
            using var message = Message.From(Marker);
            var marker = Socket.Send(RoutingId).Message(message).Async(Token);
            Assert.Equal(Marker, await ReadAsync(Marker.Length));
            await marker.Admitted.WaitAsync(Token);
            using var noExtra = CancellationTokenSource.CreateLinkedTokenSource(Token);
            noExtra.CancelAfter(TimeSpan.FromMilliseconds(100));
            await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
                _peer.GetStream().ReadAsync(new byte[1], noExtra.Token).AsTask());
            Assert.False(Token.IsCancellationRequested);
        }

        public async ValueTask DisposeAsync()
        {
            _peer.Dispose();
            await _backend.DisposeAsync();
            await Context.DisposeAsync();
            _timeout.Dispose();
        }
    }

    private sealed record Saturation(int Count, Task Admitted);
    private sealed record ReceivedFrame(ZlinkStreamHeader Header, byte[] Payload);
}
