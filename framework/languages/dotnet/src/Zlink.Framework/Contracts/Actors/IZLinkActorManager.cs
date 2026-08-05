namespace Zlink.Framework.Contracts.Actors;

public interface IZLinkActorManager
{
    IZLinkActorCreateCall Create(string actorId, string actorType);
    IZLinkActorGetOrCreateCall GetOrCreate(string actorId, string actorType);
    ValueTask<ActorRef?> FindAsync(
        string actorId,
        CancellationToken cancellationToken = default);
    ValueTask<SpotRef?> FindSpotAsync(
        string actorId,
        CancellationToken cancellationToken = default);
    ValueTask<bool> DestroyAsync(
        ActorRef actor,
        CancellationToken cancellationToken = default);
}

public interface IZLinkActorCreateCall
{
    IZLinkActorCreateCall InMesh(string meshName);
    IZLinkActorCreateCall Request(ZLinkMessage request);
    IZLinkActorCreateCall Request<TRequest>(TRequest request);
    IZLinkActorCreateCall Timeout(TimeSpan timeout);
    ValueTask<ZLinkActorCreateResult> Async(CancellationToken cancellationToken = default);
    ValueTask<ZLinkActorCreateResult> Yield(CancellationToken cancellationToken = default);
}

public interface IZLinkActorGetOrCreateCall
{
    IZLinkActorGetOrCreateCall InMesh(string meshName);
    IZLinkActorGetOrCreateCall Request(ZLinkMessage request);
    IZLinkActorGetOrCreateCall Request<TRequest>(TRequest request);
    IZLinkActorGetOrCreateCall Timeout(TimeSpan timeout);
    ValueTask<ZLinkActorCreateResult> Async(CancellationToken cancellationToken = default);
    ValueTask<ZLinkActorCreateResult> Yield(CancellationToken cancellationToken = default);
}

public abstract record ZLinkActorCreateResult
{
    private protected ZLinkActorCreateResult()
    {
    }

    public sealed record Existing(ActorRef Actor) : ZLinkActorCreateResult;

    public sealed record Created(ActorRef Actor, ZLinkMessage? Reply)
        : ZLinkActorCreateResult;

    public sealed record Rejected(ZLinkMessage? Reply) : ZLinkActorCreateResult;
}
