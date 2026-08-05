namespace Zlink.Framework.Contracts.Actors;

public interface IZLinkActorContext
{
    string ActorId { get; }

    ulong ObjectGeneration { get; }

    string MeshName { get; }

    string? SpotId { get; }

    IZLinkBoundSession BoundSession { get; }

    IZLinkActorJoinSpotCall JoinSpot(
        string spotId,
        ZLinkMessage request);

    IZLinkActorJoinSpotCall JoinSpot(string spotId)
    {
        return JoinSpot(spotId, ZLinkMessage.Empty);
    }

    IZLinkActorJoinSpotCall JoinSpot<TRequest>(
        string spotId,
        TRequest request)
    {
        return JoinSpot(spotId, ZLinkMessage.From(request));
    }

    IZLinkActorJoinEntrySpotCall JoinEntrySpot(
        ZLinkMessage request);

    IZLinkActorJoinEntrySpotCall JoinEntrySpot()
    {
        return JoinEntrySpot(ZLinkMessage.Empty);
    }

    IZLinkActorJoinEntrySpotCall JoinEntrySpot<TRequest>(
        TRequest request)
    {
        return JoinEntrySpot(ZLinkMessage.From(request));
    }
}

public readonly record struct ZLinkActorJoinOperationId(ulong High, ulong Low);

public abstract record ZLinkActorJoinCompletion
{
    private protected ZLinkActorJoinCompletion()
    {
    }

    public sealed record Accepted(
        ZLinkActorJoinOperationId OperationId,
        ActorRef Actor,
        ZLinkMessage? Reply) : ZLinkActorJoinCompletion;

    public sealed record Rejected(
        ZLinkActorJoinOperationId OperationId,
        ZLinkMessage? Reply) : ZLinkActorJoinCompletion;

    public sealed record Failed(
        ZLinkActorJoinOperationId OperationId,
        ZLinkFrameworkErrorKind Kind) : ZLinkActorJoinCompletion;
}

public interface IZLinkActorDeferredJoinCall
{
    void Defer();
}

public interface IZLinkActorJoinSpotCall : IZLinkActorDeferredJoinCall
{
    IZLinkActorJoinSpotCall Timeout(TimeSpan timeout);
}

public interface IZLinkActorJoinEntrySpotCall : IZLinkActorDeferredJoinCall
{
    IZLinkActorJoinEntrySpotCall Timeout(TimeSpan timeout);
}
