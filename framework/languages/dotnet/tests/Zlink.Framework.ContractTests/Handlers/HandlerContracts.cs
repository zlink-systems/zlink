using Zlink.Framework.ContractTests.Support;

namespace Zlink.Framework.ContractTests.Handlers;

public sealed class HandlerContracts
{
    [Fact]
    [ContractExample(
        typeof(IZLinkMessageContext),
        typeof(IZLinkHandlerFilterContext),
        typeof(IZLinkRequestHandler<,>),
        typeof(IZLinkSendHandler<>),
        typeof(IZLinkFanoutHandler<>),
        typeof(IZLinkHandlerFilter))]
    public async Task Channel_handlers_and_filters_keep_user_code_behind_typed_contracts()
    {
        var sendHandler = new PlayerJoinedSendHandler();
        var requestHandler = new AuthenticateRequestHandler();
        var publishHandler = new RoomEventPublishHandler();
        var filter = new AuditingFilter();

        await sendHandler.HandleAsync(new PlayerJoined("alice"), null!, CancellationToken.None);
        var reply = await requestHandler.HandleAsync(new Authenticate("alice"), null!, CancellationToken.None);
        await publishHandler.HandleAsync(new RoomEvent("started"), CancellationToken.None);

        await filter.InvokeAsync(
            null!,
            () => ValueTask.CompletedTask,
            CancellationToken.None);

        Assert.True(sendHandler.WasCalled);
        Assert.Equal("alice", reply.PlayerId);
        Assert.True(publishHandler.WasCalled);
    }

    [Fact]
    [ContractExample(typeof(IZLinkMessageContext))]
    public void Handler_context_exposes_only_dispatch_metadata()
    {
        var publicProperties = typeof(IZLinkMessageContext)
            .GetProperties()
            .Select(static property => property.Name)
            .Order(StringComparer.Ordinal)
            .ToArray();

        Assert.Equal(
            new[]
            {
                nameof(IZLinkMessageContext.ChannelName),
                nameof(IZLinkMessageContext.ContentType),
                nameof(IZLinkMessageContext.CorrelationId),
                nameof(IZLinkMessageContext.MeshName),
                nameof(IZLinkMessageContext.Metadata),
                nameof(IZLinkMessageContext.PacketName)
            },
            publicProperties);

        Assert.DoesNotContain(
            typeof(IZLinkHandlerFilter).Assembly.GetTypes(),
            static type => type.Name == "ZLinkHandlerInvocation");

        Assert.Equal(
            new[]
            {
                ZLinkHandlerDispatchKind.NodeDirectSend,
                ZLinkHandlerDispatchKind.NodeDirectRequest,
                ZLinkHandlerDispatchKind.ChannelSend,
                ZLinkHandlerDispatchKind.ChannelRequest,
                ZLinkHandlerDispatchKind.ClassicFanout
            },
            Enum.GetValues<ZLinkHandlerDispatchKind>());
        Assert.Equal(
            typeof(IZLinkHandlerFilterContext),
            typeof(IZLinkHandlerFilter)
                .GetMethod(nameof(IZLinkHandlerFilter.InvokeAsync))!
                .GetParameters()[0]
                .ParameterType);
    }

    private sealed record Authenticate(string PlayerId);

    private sealed record Authenticated(string PlayerId);

    private sealed record PlayerJoined(string PlayerId);

    private sealed record RoomEvent(string State);

    private sealed class AuthenticateRequestHandler : IZLinkRequestHandler<Authenticate, Authenticated>
    {
        public ValueTask<Authenticated> HandleAsync(
            Authenticate request,
            IZLinkMessageContext context,
            CancellationToken cancellationToken)
        {
            return ValueTask.FromResult(new Authenticated(request.PlayerId));
        }
    }

    private sealed class PlayerJoinedSendHandler : IZLinkSendHandler<PlayerJoined>
    {
        public bool WasCalled { get; private set; }

        public ValueTask HandleAsync(
            PlayerJoined message,
            IZLinkMessageContext context,
            CancellationToken cancellationToken)
        {
            WasCalled = true;
            return ValueTask.CompletedTask;
        }
    }

    private sealed class RoomEventPublishHandler : IZLinkFanoutHandler<RoomEvent>
    {
        public bool WasCalled { get; private set; }

        public ValueTask HandleAsync(
            RoomEvent message,
            CancellationToken cancellationToken)
        {
            WasCalled = true;
            return ValueTask.CompletedTask;
        }
    }

    private sealed class AuditingFilter : IZLinkHandlerFilter
    {
        public ValueTask InvokeAsync(
            IZLinkHandlerFilterContext context,
            ZLinkHandlerFilterNext next,
            CancellationToken cancellationToken)
        {
            return next();
        }
    }
}
