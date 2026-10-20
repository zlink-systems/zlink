namespace Zlink.Framework.Contracts.Actors;

internal abstract record ZLinkActorJoinResult
{
    private protected ZLinkActorJoinResult() { }

    public sealed record Accepted(ActorRef Actor, ZLinkMessage Reply) : ZLinkActorJoinResult;

    public sealed record Rejected(ZLinkMessage Reply) : ZLinkActorJoinResult;

    // Relay readiness transfers public completion to the target. Source
    // authority settlement only decides how to release the retained work.
    public sealed record Relayed(Func<ValueTask<ZLinkSourceSettlement>> SourceCleanup)
        : ZLinkActorJoinResult;
}
