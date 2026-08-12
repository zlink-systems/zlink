using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Messaging;

namespace Zlink.Framework.UnitTests;

public sealed class ActorClientTests
{
    [Fact]
    public void ActorReplyDecoder_MapsEmptyReplyToProtocolError()
    {
        var error = Assert.Throws<ZLinkFrameworkException>(
            () => ZLinkActorReplyDecoder.Decode<object>([]));

        Assert.Equal(ZLinkFrameworkErrorKind.ProtocolError, error.Kind);
    }

    [Fact]
    public void ActorCreationDeadline_MapsProviderTimeoutToRetryableFrameworkError()
    {
        var providerTimeout = new TimeoutException("provider operation timed out");

        var error = ZLinkActorManagerService.CreateActorCreationDeadlineException(
            "actor-1",
            providerTimeout);

        Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, error.Kind);
        Assert.Contains("actor-1", error.Message, StringComparison.Ordinal);
        Assert.Same(providerTimeout, error.InnerException);
    }

    [Fact]
    public void ActorReplyDecoder_MapsMalformedFrameToProtocolError()
    {
        using var malformed = Message.From("not-an-actor-frame");

        var error = Assert.Throws<ZLinkFrameworkException>(
            () => ZLinkActorReplyDecoder.Decode<object>([malformed]));

        Assert.Equal(ZLinkFrameworkErrorKind.ProtocolError, error.Kind);
        Assert.NotNull(error.InnerException);
    }

    [Fact]
    public void ActorReplyDecoder_MapsNonResponseKindToProtocolError()
    {
        var parts = ActorReplyParts(ZlinkStreamMessageKind.Send, "{}");
        try
        {
            var error = Assert.Throws<ZLinkFrameworkException>(
                () => ZLinkActorReplyDecoder.Decode<object>(parts));

            Assert.Equal(ZLinkFrameworkErrorKind.ProtocolError, error.Kind);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    [Theory]
    [InlineData("FutureError")]
    [InlineData("999")]
    public void ActorReplyDecoder_MapsUnknownWireErrorToProtocolError(string errorCode)
    {
        var parts = ActorReplyParts(
            ZlinkStreamMessageKind.Error,
            $"{{\"code\":\"{errorCode}\",\"message\":\"unsupported\"}}");
        try
        {
            var error = Assert.Throws<ZLinkFrameworkException>(
                () => ZLinkActorReplyDecoder.Decode<object>(parts));

            Assert.Equal(ZLinkFrameworkErrorKind.ProtocolError, error.Kind);
            Assert.Contains(errorCode, error.Message, StringComparison.Ordinal);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    [Theory]
    [InlineData("")]
    [InlineData("null")]
    [InlineData("{")]
    public void ActorReplyDecoder_MapsNullOrMalformedPayloadToDecodeFailure(string payload)
    {
        var parts = ActorReplyParts(ZlinkStreamMessageKind.Response, payload);
        try
        {
            var error = Assert.Throws<ZLinkFrameworkException>(
                () => ZLinkActorReplyDecoder.Decode<DecodedActorReply>(parts));

            Assert.Equal(ZLinkFrameworkErrorKind.ProtocolError, error.Kind);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    [Theory]
    [InlineData("")]
    [InlineData("null")]
    [InlineData("{")]
    public void ActorReplyDecoder_MapsNullOrMalformedErrorPayloadToDecodeFailure(string payload)
    {
        var parts = ActorReplyParts(ZlinkStreamMessageKind.Error, payload);
        try
        {
            var error = Assert.Throws<ZLinkFrameworkException>(
                () => ZLinkActorReplyDecoder.Decode<DecodedActorReply>(parts));

            Assert.Equal(ZLinkFrameworkErrorKind.ProtocolError, error.Kind);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    [Fact]
    public async Task AddZLinkFramework_Registers_ActorClient_With_SpotNode_And_Locations()
    {
        var services = new ServiceCollection();
        services.AddZLinkFramework(options =>
        {
            options.UseTestLocationStore();
            options.AddRouteMesh("play")
                .Listen("inproc://actor-client")
                .Channel("play")
                .Server();
        });

        await using var provider = services.BuildServiceProvider();

        Assert.NotNull(provider.GetService<IZLinkActorClient>());
    }

    [Fact]
    public async Task AddZLinkFramework_DoesNot_Register_ActorClient_Without_Locations()
    {
        var services = new ServiceCollection();
        services.AddZLinkFramework(options =>
        {
            options.AddRouteMesh("play")
                .Listen("inproc://actor-client")
                .Channel("play")
                .Server();
        });

        await using var provider = services.BuildServiceProvider();

        Assert.Null(provider.GetService<IZLinkActorClient>());
    }

    [Fact]
    public void ActorSendCall_Has_One_Async_Terminal()
    {
        var submit = Assert.Single(
            typeof(IZLinkActorSendCall).GetMethods(),
            static method => method.Name == "Async");
        Assert.Equal(typeof(ValueTask), submit.ReturnType);
        var cancellation = Assert.Single(submit.GetParameters());
        Assert.Equal(typeof(CancellationToken), cancellation.ParameterType);
        Assert.True(cancellation.HasDefaultValue);
        Assert.Empty(typeof(IZLinkActorSendCall).GetMethods().Where(static method =>
            method.Name is "Submit" or "SubmitAsync" or "TrySubmit"));
    }

    [Fact]
    public void ActorRequestCall_Has_No_Submit_Terminal()
    {
        Assert.Empty(typeof(IZLinkActorRequestCall).GetMethods().Where(static method => method.Name == "Submit"));
    }

    [Fact]
    public async Task OneWayBuilders_RejectSecondTerminalWithAlreadySubmitted()
    {
        var services = new ServiceCollection();
        var registration = new ZLinkFrameworkRegistration();
        services.AddSingleton(registration);
        await using var provider = services.BuildServiceProvider();
        var runtime = new ZLinkFrameworkRuntime(
            provider,
            null!,
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(
                provider.GetRequiredService<IServiceScopeFactory>(),
                registration));
        var session = new ZLinkSessionContext(
            runtime,
            new OneWayTestStream(),
            new OneWayTestSessionHandlerRegistry(),
            static () => ValueTask.CompletedTask,
            static _ => ValueTask.CompletedTask);

        IZLinkSendCall send = new ZLinkRouteClient(runtime)
            .SendToChannel("missing", new OneWayMessage("send"));
        IZLinkPublishCall publish = new ZLinkSpotPublisherClientService(runtime)
            .Publish("missing", "topic", new OneWayMessage("publish"));
        IZLinkActorSendCall actorSend = new ZLinkActorClient(runtime)
            .SendToActor("actor", new OneWayMessage("actor"));
        IZLinkBoundSessionSendCall boundSessionSend =
            new ZLinkBoundSessionService(runtime)
                .Create("actor")
                .Send(new OneWayMessage("bound-session"));
        IZLinkSessionSendCall sessionSend = session.Client
            .Send(new OneWayMessage("session"));
        IZLinkSessionReplyCall sessionReply = session.Client
            .Reply(new OneWayMessage("reply"));

        var terminals = new (string Contract, Func<CancellationToken, ValueTask> Async)[]
        {
            (nameof(IZLinkSendCall), send.Async),
            (nameof(IZLinkPublishCall), publish.Async),
            (nameof(IZLinkActorSendCall), actorSend.Async),
            (nameof(IZLinkBoundSessionSendCall), boundSessionSend.Async),
            (nameof(IZLinkSessionSendCall), sessionSend.Async),
            (nameof(IZLinkSessionReplyCall), sessionReply.Async)
        };

        foreach (var terminal in terminals)
        {
            using var cancellation = new CancellationTokenSource();
            cancellation.Cancel();
            _ = await Record.ExceptionAsync(
                () => terminal.Async(cancellation.Token).AsTask());

            var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(
                () => terminal.Async(CancellationToken.None).AsTask());

            Assert.Equal(
                ZLinkFrameworkErrorKind.InvalidOperation,
                error.Kind);
            Assert.True(
                error.Message.Contains(
                    "already submitted",
                    StringComparison.Ordinal),
                $"{terminal.Contract} did not report second-terminal rejection.");
        }
    }

    private static IReadOnlyList<Message> ActorReplyParts(
        ZlinkStreamMessageKind kind,
        string payload)
    {
        var header = new ZlinkStreamHeader(
            kind,
            ZlinkStreamCodec.Json,
            kind is ZlinkStreamMessageKind.Response or ZlinkStreamMessageKind.Error
                ? ZlinkStreamHeaderFlags.HasRequestSeq
                : ZlinkStreamHeaderFlags.None,
            kind is ZlinkStreamMessageKind.Response or ZlinkStreamMessageKind.Error
                ? new ZlinkStreamRequestSeq(1)
                : null,
            kind is ZlinkStreamMessageKind.Response or ZlinkStreamMessageKind.Error
                ? string.Empty
                : "packet",
            ZlinkStreamMetadata.Empty);
        return
        [
            Message.From(ZLinkStreamProtocolDefaults.EncodeHeader(header).Span),
            Message.From(payload)
        ];
    }

    private sealed record DecodedActorReply(string Value);

    private sealed record OneWayMessage(string Value);

    private sealed class OneWayTestSessionHandlerRegistry
        : IZLinkSessionHandlerRegistry
    {
        public void AddHandler<THandler>() where THandler : class { }

        public void AddHandler<THandler>(string packetName)
            where THandler : class { }

        public ValueTask<bool> TryHandleAsync(
            ZLinkSessionDispatchContext dispatch,
            ZLinkMessage payload,
            CancellationToken cancellationToken = default) =>
            ValueTask.FromResult(false);
    }

    private sealed class OneWayTestStream : IZLinkStream
    {
        public string SessionId => "one-way";

        public RoutingId? RoutingId => null;

        public string? LocalAddr => null;

        public string? RemoteAddr => null;

        public bool Write(
            ZLinkMessage payload,
            SendFlags flags = SendFlags.None) => false;

        public ValueTask CloseAsync() => ValueTask.CompletedTask;
    }
}
