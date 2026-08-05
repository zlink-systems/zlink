namespace Zlink.Framework.Contracts.Spots;

public readonly record struct ZLinkSpotActorJoinResult(bool Accepted, ZLinkMessage? Reply)
{
    public static ZLinkSpotActorJoinResult Accept(ZLinkMessage? reply = null)
    {
        var result = ZLinkSpotAcceptRejectResult.Accept(reply);
        return new ZLinkSpotActorJoinResult(result.Accepted, result.Reply);
    }

    public static ZLinkSpotActorJoinResult Accept<TReply>(TReply reply)
    {
        var result = ZLinkSpotAcceptRejectResult.Accept(reply);
        return new ZLinkSpotActorJoinResult(result.Accepted, result.Reply);
    }

    public static ZLinkSpotActorJoinResult Reject(ZLinkMessage? reply = null)
    {
        var result = ZLinkSpotAcceptRejectResult.Reject(reply);
        return new ZLinkSpotActorJoinResult(result.Accepted, result.Reply);
    }

    public static ZLinkSpotActorJoinResult Reject<TReply>(TReply reply)
    {
        var result = ZLinkSpotAcceptRejectResult.Reject(reply);
        return new ZLinkSpotActorJoinResult(result.Accepted, result.Reply);
    }
}

public readonly record struct ZLinkActorCreateResponse(bool Accepted, ZLinkMessage? Reply)
{
    public static ZLinkActorCreateResponse Accept(ZLinkMessage? reply = null)
    {
        var result = ZLinkSpotAcceptRejectResult.Accept(reply);
        return new ZLinkActorCreateResponse(result.Accepted, result.Reply);
    }

    public static ZLinkActorCreateResponse Accept<TReply>(TReply reply)
    {
        var result = ZLinkSpotAcceptRejectResult.Accept(reply);
        return new ZLinkActorCreateResponse(result.Accepted, result.Reply);
    }

    public static ZLinkActorCreateResponse Reject(ZLinkMessage? reply = null)
    {
        var result = ZLinkSpotAcceptRejectResult.Reject(reply);
        return new ZLinkActorCreateResponse(result.Accepted, result.Reply);
    }

    public static ZLinkActorCreateResponse Reject<TReply>(TReply reply)
    {
        var result = ZLinkSpotAcceptRejectResult.Reject(reply);
        return new ZLinkActorCreateResponse(result.Accepted, result.Reply);
    }
}

public interface IZLinkSpot
{
    IZLinkSpotContext Context { get; }

    void Configure()
    {
    }

    ValueTask<ZLinkSpotCreateResponse> OnCreateAsync(
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        return ValueTask.FromResult(ZLinkSpotCreateResponse.Accept());
    }

    ValueTask OnInitializeAsync(CancellationToken cancellationToken)
    {
        return ValueTask.CompletedTask;
    }

    ValueTask OnClosingAsync(
        ZLinkSpotClosingContext context,
        CancellationToken cleanupCancellationToken)
    {
        return ValueTask.CompletedTask;
    }

    ValueTask OnRelocationReadyCompletedAsync(
        ZLinkSpotRelocationReadyCompletion completion,
        CancellationToken cancellationToken)
    {
        return ValueTask.CompletedTask;
    }
}

public enum ZLinkSpotCloseReason
{
    ExplicitClose = 0,
    HostShutdown = 1,
    RelocationOut = 2,
    IdleEvicted = 3
}

public readonly record struct ZLinkSpotClosingContext(
    ZLinkSpotCloseReason Reason,
    DateTimeOffset Deadline);

public enum ZLinkSpotRelocationReadyOutcome
{
    Continued = 0,
    Relocated = 1
}

public readonly record struct ZLinkSpotRelocationReadyCompletion(
    ZLinkSpotRelocationReadyOutcome Outcome);

public interface IZLinkSpotRelocationReadyCall
{
    void Defer();
}

public interface IZLinkSpotActorMembershipLifecycle<TActor>
    where TActor : IZLinkActor
{
    ValueTask OnJoinedActorAsync(
        TActor actor,
        CancellationToken cancellationToken);

    ValueTask OnLeaveActorAsync(
        TActor actor,
        CancellationToken cancellationToken);

    ValueTask OnDisconnectActorAsync(
        TActor actor,
        CancellationToken cancellationToken)
    {
        return ValueTask.CompletedTask;
    }
}

public interface IZLinkUserSpotActorLifecycle<TActor>
    : IZLinkSpotActorMembershipLifecycle<TActor>
    where TActor : IZLinkActor
{
    ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken cancellationToken);
}

public interface IZLinkSpot<TActor> : IZLinkSpot, IZLinkUserSpotActorLifecycle<TActor>
    where TActor : IZLinkActor;

public interface IZLinkActorHandlerRegistry
{
    void AddHandler<THandler>()
        where THandler : class;

    void AddHandler<THandler>(string packetName)
        where THandler : class;

    void AddActorPacket<THandler, TActor>()
        where THandler : class
        where TActor : IZLinkActor;

    void AddActorPacket<THandler, TActor>(string packetName)
        where THandler : class
        where TActor : IZLinkActor;
}

public interface IZLinkSpotHandlerRegistry : IZLinkActorHandlerRegistry
{
    void AddPacket<THandler>()
        where THandler : class;

    void AddSubscribe<THandler>(string channelName, string topic)
        where THandler : class;
}

public interface IZLinkInstanceSpotHandlerRegistry
{
    void AddPacket<THandler>()
        where THandler : class;
}

public interface IZLinkSpotOutbound
{
    IZLinkSpotSendCall SendToSpot<TMessage>(
        string spotId,
        TMessage message);

    IZLinkSpotRequestCall RequestToSpot<TRequest>(
        string spotId,
        TRequest request);

    IZLinkPublishCall Publish<TEvent>(
        string channelName,
        string topic,
        TEvent message);

    IZLinkSendCall SendToChannel<TMessage>(
        string channelName,
        TMessage message);

    IZLinkRequestCall RequestToChannel<TRequest>(
        string channelName,
        TRequest request);
}

public interface IZLinkSpotClient
{
    IZLinkSpotSendCall SendToSpot<TMessage>(string spotId, TMessage message);

    IZLinkSpotRequestCall RequestToSpot<TRequest>(string spotId, TRequest request);

}

public interface IZLinkSpotSendCall : IZLinkMetadataCall<IZLinkSpotSendCall>
{
    IZLinkSpotSendCall InstanceSpot();

    IZLinkSpotSendCall InstanceSpot(string instanceSpotType);

    IZLinkSpotSendCall InMesh(string meshName);

    ValueTask Async(CancellationToken cancellationToken = default);
}

public interface IZLinkSpotRequestCall : IZLinkMetadataCall<IZLinkSpotRequestCall>
{
    IZLinkSpotRequestCall InstanceSpot();

    IZLinkSpotRequestCall InstanceSpot(string instanceSpotType);

    IZLinkSpotRequestCall InMesh(string meshName);

    IZLinkSpotRequestCall Timeout(TimeSpan timeout);

    ValueTask<TReply> Async<TReply>(CancellationToken cancellationToken = default);

    ValueTask<TReply> Yield<TReply>(CancellationToken cancellationToken = default);
}

public interface IZLinkSpotCommonContext
{
    string MeshName { get; }

    string SpotId { get; }

    ulong ObjectGeneration { get; }

    RoutingId NodeRid { get; }

    IZLinkSpotOutbound Outbound { get; }

    ValueTask<IZLinkTimer> AddTimer<THandler>(
        string name,
        TimeSpan period,
        ZLinkTimerOptions? options = null,
        CancellationToken cancellationToken = default)
        where THandler : class;

    IZLinkWorkerCall<TResult> RunCpuWorker<TResult>(
        Func<CancellationToken, TResult> work);

    IZLinkWorkerCall<TResult> RunIoWorker<TResult>(
        Func<CancellationToken, ValueTask<TResult>> work);
}

public interface IZLinkSpotContext : IZLinkSpotCommonContext
{
    IZLinkSpotHandlerRegistry Handlers { get; }

    IZLinkSpotRelocationReadyCall RelocationReady();

    ValueTask LeaveActorAsync(
        IZLinkActor actor,
        CancellationToken cancellationToken = default);

    ValueTask<bool> CloseAsync(
        CancellationToken cancellationToken = default);
}

public interface IZLinkEntrySpot
{
    IZLinkEntrySpotContext Context { get; }

    void Configure()
    {
    }

    ValueTask OnInitializeAsync(CancellationToken cancellationToken)
    {
        return ValueTask.CompletedTask;
    }

    ValueTask OnClosingAsync(
        ZLinkSpotClosingContext context,
        CancellationToken cleanupCancellationToken)
    {
        return ValueTask.CompletedTask;
    }
}

public interface IZLinkEntrySpot<TActor>
    : IZLinkEntrySpot, IZLinkSpotActorMembershipLifecycle<TActor>
    where TActor : IZLinkActor
{
    ValueTask<ZLinkActorCreateResponse> OnCreateActorAsync(
        TActor actor,
        ZLinkMessage createRequest,
        CancellationToken cancellationToken)
    {
        return ValueTask.FromResult(ZLinkActorCreateResponse.Accept());
    }
}

public interface IZLinkEntrySpotContext : IZLinkSpotCommonContext
{
    IZLinkSpotHandlerRegistry Handlers { get; }

    ValueTask DestroyActorAsync(
        IZLinkActor actor,
        CancellationToken cancellationToken = default);
}

public interface IZLinkSpotActorSendHandler<TSpot, TActor, in TMessage>
    where TSpot : class
    where TActor : IZLinkActor
{
    ValueTask HandleAsync(
        TSpot spot,
        TActor actor,
        IZLinkMessageContext context,
        TMessage message,
        CancellationToken cancellationToken);
}

public interface IZLinkSpotActorRequestHandler<TSpot, TActor, in TRequest, TReply>
    where TSpot : class
    where TActor : IZLinkActor
{
    ValueTask<TReply> HandleAsync(
        TSpot spot,
        TActor actor,
        IZLinkMessageContext context,
        TRequest request,
        CancellationToken cancellationToken);
}

public interface IZLinkEntrySpotActorSendHandler<TEntrySpot, TActor, in TMessage>
    where TEntrySpot : class, IZLinkEntrySpot
    where TActor : IZLinkActor
{
    ValueTask HandleAsync(
        TEntrySpot entrySpot,
        TActor actor,
        IZLinkMessageContext context,
        TMessage message,
        CancellationToken cancellationToken);
}

public interface IZLinkEntrySpotActorRequestHandler<TEntrySpot, TActor, in TRequest, TReply>
    where TEntrySpot : class, IZLinkEntrySpot
    where TActor : IZLinkActor
{
    ValueTask<TReply> HandleAsync(
        TEntrySpot entrySpot,
        TActor actor,
        IZLinkMessageContext context,
        TRequest request,
        CancellationToken cancellationToken);
}
