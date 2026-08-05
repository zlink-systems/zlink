using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Handlers;

namespace Zlink.Framework.UnitTests;

public abstract class RegistrationValidationSupport
{
    protected sealed class TestFilter : IZLinkHandlerFilter
    {
        public ValueTask InvokeAsync(
            IZLinkHandlerFilterContext context,
            ZLinkHandlerFilterNext next,
            CancellationToken cancellationToken)
        {
            return next();
        }
    }

    protected sealed record TestChannelRequest(string Value);

    protected sealed record TestChannelReply(string Value);

    [ZLinkHandlerGroup("validation-request")]
    protected sealed class TestChannelRequestHandler
        : IZLinkRequestHandler<TestChannelRequest, TestChannelReply>
    {
        public ValueTask<TestChannelReply> HandleAsync(
            TestChannelRequest request,
            IZLinkMessageContext context,
            CancellationToken cancellationToken)
        {
            _ = context;
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.FromResult(new TestChannelReply(request.Value));
        }
    }

    protected sealed class AlternateTestChannelRequestHandler
        : IZLinkRequestHandler<TestChannelRequest, TestChannelReply>
    {
        public ValueTask<TestChannelReply> HandleAsync(
            TestChannelRequest request,
            IZLinkMessageContext context,
            CancellationToken cancellationToken)
        {
            _ = context;
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.FromResult(new TestChannelReply(request.Value));
        }
    }

    protected sealed record TestSendMessage(string Value);

    protected sealed class AlternateTestSendHandler : IZLinkSendHandler<TestSendMessage>
    {
        public ValueTask HandleAsync(
            TestSendMessage message,
            IZLinkMessageContext context,
            CancellationToken cancellationToken)
        {
            _ = message;
            _ = context;
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.CompletedTask;
        }
    }

    protected sealed record TestPublishedEvent(string Value);

    [ZLinkHandlerGroup("validation-publish")]
    protected sealed class TestPublishHandler : IZLinkFanoutHandler<TestPublishedEvent>
    {
        public ValueTask HandleAsync(
            TestPublishedEvent message,
            CancellationToken cancellationToken)
        {
            _ = message;
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.CompletedTask;
        }
    }

    protected sealed record TestRouteRequest(string Value);

    protected sealed record TestRouteReply(string Value);

    [ZLinkHandlerGroup("validation-route")]
    protected sealed class TestRouteRequestHandler
        : IZLinkRouteRequestHandler<TestRouteRequest, TestRouteReply>
    {
        public ValueTask<TestRouteReply> HandleAsync(
            TestRouteRequest request,
            ZLinkRouteMessageContext context,
            CancellationToken cancellationToken)
        {
            _ = context;
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.FromResult(new TestRouteReply(request.Value));
        }
    }

    protected sealed class AlternateTestRouteRequestHandler
        : IZLinkRouteRequestHandler<TestRouteRequest, TestRouteReply>
    {
        public ValueTask<TestRouteReply> HandleAsync(
            TestRouteRequest request,
            ZLinkRouteMessageContext context,
            CancellationToken cancellationToken)
        {
            _ = context;
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.FromResult(new TestRouteReply(request.Value));
        }
    }

    protected sealed class TestHeaderSession : IZLinkSession
    {
        public TestHeaderSession(IZLinkSessionContext context)
        {
            Context = context;
        }

        public IZLinkSessionContext Context { get; }

        public int AttributedHandledCount { get; private set; }

        [ZLinkStreamPacket]
        public ValueTask HandleAttributedAsync(
            AttributedSessionPacketMessage message,
            ZLinkSessionDispatchContext dispatch,
            CancellationToken cancellationToken)
        {
            _ = message;
            _ = dispatch;
            cancellationToken.ThrowIfCancellationRequested();
            AttributedHandledCount++;
            return ValueTask.CompletedTask;
        }

        public ValueTask OnConnectedAsync(CancellationToken cancellationToken)
        {
            return ValueTask.CompletedTask;
        }

        public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken)
        {
            return ValueTask.CompletedTask;
        }

        public ValueTask OnErrorAsync(ZLinkStreamError error, CancellationToken cancellationToken)
        {
            return ValueTask.CompletedTask;
        }

        public ValueTask OnDispatchAsync(
            ZLinkSessionDispatchContext dispatch,
            Message body,
            CancellationToken cancellationToken)
        {
            return ValueTask.CompletedTask;
        }
    }

    protected interface ITestSessionDependencyHandler
    {
    }

    protected sealed class TestSessionDependencyHandler : ITestSessionDependencyHandler
    {
    }

    protected sealed class TestSessionWithEnumerableHandlers(
        IZLinkSessionContext context,
        IEnumerable<ITestSessionDependencyHandler> handlers) : IZLinkSession
    {
        public IReadOnlyCollection<ITestSessionDependencyHandler> Handlers { get; } = handlers.ToArray();
        public IZLinkSessionContext Context { get; } = context;

        public ValueTask OnConnectedAsync(CancellationToken cancellationToken)
        {
            return ValueTask.CompletedTask;
        }

        public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken)
        {
            return ValueTask.CompletedTask;
        }

        public ValueTask OnErrorAsync(ZLinkStreamError error, CancellationToken cancellationToken)
        {
            return ValueTask.CompletedTask;
        }

        public ValueTask OnDispatchAsync(
            ZLinkSessionDispatchContext dispatch,
            Message body,
            CancellationToken cancellationToken)
        {
            return ValueTask.CompletedTask;
        }
    }

    protected sealed record TestSessionPacketMessage;

    protected sealed record AttributedSessionPacketMessage;

    protected class TestSessionPacketContext : IZLinkSessionContext
    {
        public int HandledCount { get; private set; }

        public void MarkHandled()
        {
            HandledCount++;
        }

        public string SessionId => "test-session";

        public RoutingId? RoutingId => Systems.Zlink.RoutingId.From("test-session-route");

        public string? LocalAddr => "tcp://127.0.0.1:9100";

        public string? RemoteAddr => "tcp://127.0.0.1:9101";

        public IZLinkSessionClient Client => throw new NotSupportedException();

        public IZLinkSessionActors Actors => throw new NotSupportedException();

        public IZLinkSessionHandlerRegistry Handlers => throw new NotSupportedException();

        public ValueTask CloseAsync() => ValueTask.CompletedTask;
    }

    protected sealed class TestSessionPacketHandler : IZLinkSessionPacketHandler<TestSessionPacketContext, TestSessionPacketMessage>
    {
        public ValueTask HandleAsync(
            TestSessionPacketContext context,
            ZLinkSessionDispatchContext dispatch,
            TestSessionPacketMessage message,
            CancellationToken cancellationToken)
        {
            _ = dispatch;
            _ = message;
            cancellationToken.ThrowIfCancellationRequested();
            context.MarkHandled();
            return ValueTask.CompletedTask;
        }
    }

    protected sealed class AsyncSessionHandlerLifetime
    {
        public List<object> Invocations { get; } = [];

        public int DisposeCount { get; set; }
    }

    protected sealed record AsyncDisposableSessionPacketMessage;

    protected sealed class AsyncDisposableSessionPacketHandler(AsyncSessionHandlerLifetime lifetime)
        : IZLinkSessionPacketHandler<TestSessionPacketContext, AsyncDisposableSessionPacketMessage>, IAsyncDisposable
    {
        public ValueTask HandleAsync(
            TestSessionPacketContext context,
            ZLinkSessionDispatchContext dispatch,
            AsyncDisposableSessionPacketMessage message,
            CancellationToken cancellationToken)
        {
            _ = context;
            _ = dispatch;
            _ = message;
            cancellationToken.ThrowIfCancellationRequested();
            lifetime.Invocations.Add(this);
            return ValueTask.CompletedTask;
        }

        public ValueTask DisposeAsync()
        {
            lifetime.DisposeCount++;
            return ValueTask.CompletedTask;
        }
    }

    protected sealed class TestSessionWithConfiguredPacketHandler(
        IZLinkSessionContext context) : IZLinkSession
    {
        public IZLinkSessionContext Context { get; } = context;

        public void Configure()
        {
            Context.Handlers.AddHandler<TestSessionPacketHandler>();
        }

        public ValueTask OnConnectedAsync(CancellationToken cancellationToken)
        {
            return ValueTask.CompletedTask;
        }

        public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken)
        {
            return ValueTask.CompletedTask;
        }

        public ValueTask OnErrorAsync(ZLinkStreamError error, CancellationToken cancellationToken)
        {
            return ValueTask.CompletedTask;
        }

        public ValueTask OnDispatchAsync(
            ZLinkSessionDispatchContext dispatch,
            Message body,
            CancellationToken cancellationToken)
        {
            return ValueTask.CompletedTask;
        }
    }

    protected sealed record DuplicateSessionPacketMessage;

    protected sealed class DuplicateSessionPacketContext : TestSessionPacketContext;

    protected sealed class DuplicateSessionPacketHandler
        : IZLinkSessionPacketHandler<DuplicateSessionPacketContext, DuplicateSessionPacketMessage>
    {
        public ValueTask HandleAsync(
            DuplicateSessionPacketContext context,
            ZLinkSessionDispatchContext dispatch,
            DuplicateSessionPacketMessage message,
            CancellationToken cancellationToken)
        {
            return ValueTask.CompletedTask;
        }
    }

    protected sealed class
        SecondDuplicateSessionPacketHandler
        : IZLinkSessionPacketHandler<DuplicateSessionPacketContext, DuplicateSessionPacketMessage>
    {
        public ValueTask HandleAsync(
            DuplicateSessionPacketContext context,
            ZLinkSessionDispatchContext dispatch,
            DuplicateSessionPacketMessage message,
            CancellationToken cancellationToken)
        {
            return ValueTask.CompletedTask;
        }
    }

    protected sealed class TestSpot(IZLinkSpotContext context) : IZLinkSpot
    {
        public IZLinkSpotContext Context { get; } = context;
    }

    protected sealed class TestInstanceSpot : IZLinkInstanceSpot
    {
        public IZLinkInstanceSpotContext Context => null!;
    }

    protected sealed class OtherTestSpot(IZLinkSpotContext context) : IZLinkSpot
    {
        public IZLinkSpotContext Context { get; } = context;
    }

    protected sealed class TestEntrySpot(IZLinkEntrySpotContext context) : IZLinkEntrySpot
    {
        public IZLinkEntrySpotContext Context { get; } = context;
    }

    protected sealed class TestActorFactory : IZLinkActorFactory<TestActor>
    {
        public ValueTask<TestActor> CreateAsync(
            IZLinkActorContext context,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.FromResult(new TestActor(context));
        }
    }

    protected sealed class TestActorRelocationAdapter : IZLinkActorRelocationAdapter<TestActor>
    {
        public ValueTask<byte[]> CaptureAsync(
            TestActor actor,
            CancellationToken cancellationToken) =>
            ValueTask.FromResult(Array.Empty<byte>());

        public ValueTask RestoreAsync(
            TestActor actor,
            ReadOnlyMemory<byte> payload,
            CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;
    }

    protected sealed class TestActor(IZLinkActorContext context) : IZLinkActor
    {
        public IZLinkActorContext Context { get; } = context;
    }

    private protected sealed class TestRelocationStore : IZLinkRelocationRepository
    {
        public ValueTask<ZLinkRelocationStored> PutRelocationAsync(
            ReadOnlyMemory<byte> payload,
            TimeSpan retention,
            CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        public ValueTask<ZLinkRelocationStored> PutRelocationAtAsync(
            string reference,
            ReadOnlyMemory<byte> payload,
            TimeSpan retention,
            CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        public ValueTask<ZLinkRelocationReadResult> GetRelocationAsync(
            string reference,
            CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        public ValueTask<ZLinkRelocationRenewResult> RenewRelocationAsync(
            string reference,
            TimeSpan retention,
            CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        public ValueTask<ZLinkRelocationDeleteResult> DeleteRelocationAsync(
            string reference,
            CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();
    }
}
