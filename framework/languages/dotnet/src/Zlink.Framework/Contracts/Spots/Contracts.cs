namespace Zlink.Framework.Contracts.Spots;

public enum ZLinkSpotCreateState
{
    Existing = 0,
    Created = 1,
    Rejected = 2
}

public readonly record struct ZLinkSpotCreateResponse(bool Accepted, ZLinkMessage? Reply)
{
    public static ZLinkSpotCreateResponse Accept(ZLinkMessage? reply = null)
    {
        var result = ZLinkSpotAcceptRejectResult.Accept(reply);
        return new ZLinkSpotCreateResponse(result.Accepted, result.Reply);
    }

    public static ZLinkSpotCreateResponse Accept<TReply>(TReply reply)
    {
        var result = ZLinkSpotAcceptRejectResult.Accept(reply);
        return new ZLinkSpotCreateResponse(result.Accepted, result.Reply);
    }

    public static ZLinkSpotCreateResponse Reject(ZLinkMessage? reply = null)
    {
        var result = ZLinkSpotAcceptRejectResult.Reject(reply);
        return new ZLinkSpotCreateResponse(result.Accepted, result.Reply);
    }

    public static ZLinkSpotCreateResponse Reject<TReply>(TReply reply)
    {
        var result = ZLinkSpotAcceptRejectResult.Reject(reply);
        return new ZLinkSpotCreateResponse(result.Accepted, result.Reply);
    }
}

public readonly record struct SpotRef(
    string SpotId,
    ulong ObjectGeneration,
    string MeshName,
    RoutingId NodeRid);

public readonly record struct ZLinkSpotCreateResult(
    SpotRef Spot,
    ZLinkSpotCreateState State,
    ZLinkMessage? Reply);

public interface IZLinkSpotManager
{
    IZLinkSpotCreateCall Create(string spotType);
    IZLinkSpotGetOrCreateCall GetOrCreate(string spotId, string spotType);
    ValueTask<SpotRef?> FindAsync(string spotId,
        CancellationToken cancellationToken = default);
    ValueTask<bool> CloseAsync(
        SpotRef spot,
        CancellationToken cancellationToken = default);
}

public interface IZLinkSpotCreateCall
{
    IZLinkSpotCreateCall InMesh(string meshName);
    IZLinkSpotCreateCall Request(ZLinkMessage request);
    IZLinkSpotCreateCall Request<TRequest>(TRequest request);
    IZLinkSpotCreateCall Timeout(TimeSpan timeout);
    ValueTask<ZLinkSpotCreateResult> Async(
        CancellationToken cancellationToken = default);
    ValueTask<ZLinkSpotCreateResult> Yield(
        CancellationToken cancellationToken = default);
}

public interface IZLinkSpotGetOrCreateCall
{
    IZLinkSpotGetOrCreateCall InMesh(string meshName);
    IZLinkSpotGetOrCreateCall Request(ZLinkMessage request);
    IZLinkSpotGetOrCreateCall Request<TRequest>(TRequest request);
    IZLinkSpotGetOrCreateCall Timeout(TimeSpan timeout);
    ValueTask<ZLinkSpotCreateResult> Async(
        CancellationToken cancellationToken = default);
    ValueTask<ZLinkSpotCreateResult> Yield(
        CancellationToken cancellationToken = default);
}

public interface IZLinkSpotPublisherClient
{
    IZLinkPublishCall Publish<TEvent>(
        string channelName,
        string topic,
        TEvent message);
}

public interface IZLinkSpotPacketHandler<TSpot, in TMessage>
    where TSpot : class
{
    ValueTask HandleAsync(
        TSpot spot,
        TMessage message,
        CancellationToken cancellationToken);
}

public interface IZLinkSpotRequestHandler<TSpot, in TRequest, TReply>
    where TSpot : class
{
    ValueTask<TReply> HandleAsync(
        TSpot spot,
        TRequest request,
        CancellationToken cancellationToken);
}

public interface IZLinkSpotSubscriptionHandler<TSpot, in TEvent>
    where TSpot : class
{
    ValueTask HandleAsync(
        TSpot spot,
        TEvent message,
        ZLinkPublishMessageContext context,
        CancellationToken cancellationToken);
}

public interface IZLinkSpotTimerHandler<TSpot>
    where TSpot : class
{
    ValueTask HandleAsync(
        TSpot spot,
        ZLinkTimerTick tick,
        CancellationToken cancellationToken);
}
