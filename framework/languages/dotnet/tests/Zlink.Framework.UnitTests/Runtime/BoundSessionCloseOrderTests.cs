using System.Buffers.Binary;
using System.Net.Sockets;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Systems.Zlink.Stream.Connector.Contracts;
using Xunit;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Streams;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Streams;

namespace Zlink.Framework.UnitTests.BoundSessionDisconnect;

public sealed class BoundSessionCloseOrderTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Earlier_reply_or_push_precedes_close(bool push)
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddZLinkFramework(o =>
        {
            o.DisableImplicitHandlerAutoRegistration();
            o.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            o.AddLocationStore(new ZLinkInMemoryProviderLocationStore());
            var objects = o.AddRouteMesh("disconnect")
                .Listen("tcp://127.0.0.1:0")
                .Objects()
                .Server();
            objects.AddEntrySpot<DEntry>();
            objects.AddActorFactory<DActor, DFactory>("actor", f => f.DisableRelocation());
            o.AddStreamNode("clients")
                .Bind("tcp://127.0.0.1:0")
                .EnableActorDispatch()
                .AddSession<DSession>();
        });
        using var app = builder.Build();
        await app.StartAsync();
        var traceDirectory = Environment.GetEnvironmentVariable("ZLINK_TEST_FLOW_DIRECTORY");
        using var flow = traceDirectory is null
            ? null
            : new TestHostMessageFlowListener(Path.Combine(traceDirectory, $"ordered-{push}.flow"));
        var failures = new List<string>();
        try
        {
            var endpoint = app
                .Services.GetRequiredService<IZLinkFrameworkRuntime>()
                .GetListenerStatus(ZLinkListenerKind.Stream, "clients")
                .Endpoint;
            var actors = app.Services.GetRequiredService<IZLinkActorClient>();
            for (var iteration = 0; iteration < 40; iteration++)
            {
                var actor = "ordered:" + Guid.NewGuid();
                using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(5));
                using var peer = new TcpClient();
                var address = new Uri(endpoint);
                await peer.ConnectAsync(address.Host, address.Port, deadline.Token);
                var stream = peer.GetStream();
                await SendRawRequestAsync(stream, new DLogin(actor), 1, deadline.Token);
                while (true)
                {
                    var login = await ReadRawFrameAsync(stream, deadline.Token);
                    Assert.NotNull(login);
                    if (login.Value.Header.Kind == ZlinkStreamMessageKind.Control)
                        continue;
                    Assert.True(
                        login.Value.Header.Kind == ZlinkStreamMessageKind.Response,
                        $"Unexpected login reply {login.Value.Header.Kind}: {System.Text.Encoding.UTF8.GetString(login.Value.Payload)}"
                    );
                    Assert.Equal(new ZlinkStreamRequestSeq(1), login.Value.Header.RequestSeq);
                    break;
                }
                // Raw reads measure frame/EOF order independently of connector callbacks.
                var pushSubmission = new TaskCompletionSource<Task>(
                    TaskCreationOptions.RunContinuationsAsynchronously
                );
                var disconnectSubmission = new TaskCompletionSource<Task>(
                    TaskCreationOptions.RunContinuationsAsynchronously
                );
                var handlerReached = new TaskCompletionSource(
                    TaskCreationOptions.RunContinuationsAsynchronously
                );
                var continueHandler = new TaskCompletionSource(
                    TaskCreationOptions.RunContinuationsAsynchronously
                );
                DOrderedHandler.PushSubmitted = push
                    ? task => pushSubmission.TrySetResult(task)
                    : null;
                DOrderedHandler.DisconnectSubmitted = push
                    ? task => disconnectSubmission.TrySetResult(task)
                    : null;
                DOrderedHandler.BeforeReplyOrPush = async cancellationToken =>
                {
                    handlerReached.TrySetResult();
                    await continueHandler.Task.WaitAsync(cancellationToken);
                };
                try
                {
                    await SendRawRequestAsync(stream, new DOrdered(push), 2, deadline.Token);
                    await handlerReached.Task.WaitAsync(deadline.Token);
                    Task<Pong>? closeReply = null;
                    if (!push)
                    {
                        var close = actors.RequestToActor(actor, new DDrop()).Async<Pong>();
                        closeReply = close.AsTask();
                        await close.Admitted.AsTask().WaitAsync(deadline.Token);
                    }
                    continueHandler.TrySetResult();
                    var events = new List<string>();
                    while (await ReadRawFrameAsync(stream, deadline.Token) is { } received)
                    {
                        if (received.Header.Kind == ZlinkStreamMessageKind.Control)
                            continue;
                        if (
                            received.Header.Kind == ZlinkStreamMessageKind.Send
                            && received.Header.Name == nameof(Pong)
                        )
                        {
                            Assert.NotNull(
                                ZLinkFrameworkJsonPayloadCodec.Deserialize<Pong>(received.Payload)
                            );
                            events.Add("push");
                        }
                        else if (
                            received.Header.Kind == ZlinkStreamMessageKind.Response
                            && received.Header.RequestSeq == new ZlinkStreamRequestSeq(2)
                        )
                        {
                            Assert.NotNull(
                                ZLinkFrameworkJsonPayloadCodec.Deserialize<Pong>(received.Payload)
                            );
                            events.Add("reply");
                        }
                        else
                            events.Add(received.Header.Kind.ToString());
                    }
                    events.Add("eof");
                    var expected = push ? "push" : "reply";
                    var expectedPosition = events.IndexOf(expected);
                    var eofPosition = events.IndexOf("eof");
                    if (expectedPosition < 0 || eofPosition < 0 || expectedPosition >= eofPosition)
                        failures.Add(
                            $"iteration={iteration} expected={expected} before eof actual={string.Join(',', events)}"
                        );
                    if (closeReply is not null)
                        await closeReply.WaitAsync(deadline.Token);
                    if (push)
                    {
                        try
                        {
                            var send = await pushSubmission.Task.WaitAsync(deadline.Token);
                            await send.WaitAsync(deadline.Token);
                            var disconnect = await disconnectSubmission.Task.WaitAsync(
                                deadline.Token
                            );
                            await disconnect.WaitAsync(deadline.Token);
                        }
                        catch (Exception error)
                        {
                            failures.Add(
                                $"iteration={iteration} push/disconnect terminal: {error.GetType().Name}: {error.Message}"
                            );
                        }
                    }
                }
                finally
                {
                    continueHandler.TrySetResult();
                }
                DOrderedHandler.BeforeReplyOrPush = null;
                DOrderedHandler.PushSubmitted = null;
                DOrderedHandler.DisconnectSubmitted = null;
            }
        }
        finally
        {
            DOrderedHandler.BeforeReplyOrPush = null;
            DOrderedHandler.PushSubmitted = null;
            DOrderedHandler.DisconnectSubmitted = null;
            await app.StopAsync();
        }
        Assert.True(failures.Count == 0, string.Join(Environment.NewLine, failures));
    }

    private static async Task SendRawRequestAsync<T>(
        NetworkStream stream,
        T message,
        ulong sequence,
        CancellationToken ct
    )
    {
        var header = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Request,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.HasRequestSeq,
            new ZlinkStreamRequestSeq(sequence),
            typeof(T).Name,
            ZlinkStreamMetadata.Empty
        );
        var frame = ZLinkStreamFrameCodec.Encode(
            ZLinkStreamProtocolDefaults.EncodeHeader(header).Span,
            ZLinkFrameworkJsonPayloadCodec.Serialize(message)
        );
        await stream.WriteAsync(frame, ct);
    }

    private static async Task<(ZlinkStreamHeader Header, byte[] Payload)?> ReadRawFrameAsync(
        NetworkStream stream,
        CancellationToken ct
    )
    {
        var prefix = new byte[ZLinkStreamFrameCodec.PrefixSize];
        if (await stream.ReadAsync(prefix.AsMemory(0, 1), ct) == 0)
            return null;
        await stream.ReadExactlyAsync(prefix.AsMemory(1), ct);
        var length = checked(
            prefix.Length
            + BinaryPrimitives.ReadUInt16BigEndian(prefix)
            + (int)BinaryPrimitives.ReadUInt32BigEndian(prefix.AsSpan(2))
        );
        Assert.InRange(length, prefix.Length, 65536);
        var frame = new byte[length];
        prefix.CopyTo(frame, 0);
        await stream.ReadExactlyAsync(frame.AsMemory(prefix.Length), ct);
        return DecodeRawFrame(frame);
    }

    private static (ZlinkStreamHeader Header, byte[] Payload) DecodeRawFrame(byte[] frame)
    {
        Assert.True(ZLinkStreamFrameCodec.TryDecode(frame, out var header, out var payload));
        return (ZLinkStreamProtocolDefaults.DecodeHeader(header.ToArray()), payload.ToArray());
    }
}

public sealed record DOrdered(bool Push);

public sealed class DOrderedHandler
    : IZLinkEntrySpotActorRequestHandler<DEntry, DActor, DOrdered, Pong>
{
    public static Func<CancellationToken, ValueTask>? BeforeReplyOrPush { get; set; }
    public static Action<Task>? PushSubmitted { get; set; }
    public static Action<Task>? DisconnectSubmitted { get; set; }

    public async ValueTask<Pong> HandleAsync(
        DEntry spot,
        DActor actor,
        IZLinkMessageContext context,
        DOrdered request,
        CancellationToken ct
    )
    {
        if (BeforeReplyOrPush is { } beforeReplyOrPush)
            await beforeReplyOrPush(ct);
        if (request.Push)
        {
            var send = actor.Context.BoundSession.Send(new Pong()).Async().AsTask();
            PushSubmitted?.Invoke(send);
            var disconnect = actor.Context.BoundSession.DisconnectAsync(ct).AsTask();
            DisconnectSubmitted?.Invoke(disconnect);
        }
        return new();
    }
}
