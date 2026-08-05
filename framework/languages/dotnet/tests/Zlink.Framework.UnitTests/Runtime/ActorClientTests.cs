using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime;
using Zlink.Framework.AspNetCore;

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
    public void EveryOneWayBuilder_UsesSingleSubmissionGate()
    {
        var terminalInterfaces = new[]
        {
            typeof(IZLinkSendCall),
            typeof(IZLinkPublishCall),
            typeof(IZLinkActorSendCall),
            typeof(IZLinkBoundSessionSendCall),
            typeof(IZLinkSessionSendCall),
            typeof(IZLinkSessionReplyCall)
        };
        var builders = typeof(ZLinkActorClient).Assembly
            .GetTypes()
            .Where(type => !type.IsAbstract
                           && terminalInterfaces.Any(contract => contract.IsAssignableFrom(type)))
            .ToArray();

        Assert.NotEmpty(builders);
        Assert.All(builders, type => Assert.True(
            HasSubmissionGate(type),
            $"{type.FullName} does not enforce single-use submission."));
    }

    [Fact]
    public void OneWayGate_RejectsSecondTerminalWithAlreadySubmitted()
    {
        var gate = new ZLinkOneWayCallGate("Actor send");
        gate.Claim();

        var error = Assert.Throws<ZLinkFrameworkException>(gate.Claim);

        Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, error.Kind);
    }

    private static bool HasSubmissionGate(Type type)
    {
        for (var current = type; current is not null; current = current.BaseType)
            if (current.GetFields(
                    System.Reflection.BindingFlags.Instance
                    | System.Reflection.BindingFlags.NonPublic)
                .Any(field => field.FieldType == typeof(ZLinkOneWayCallGate)))
                return true;

        return false;
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
}
