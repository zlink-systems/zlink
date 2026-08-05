namespace Zlink.Framework.Contracts.Actors;

internal abstract record ZLinkActorJoinResult
{
    private protected ZLinkActorJoinResult()
    {
    }

    public sealed record Accepted(ActorRef Actor, ZLinkMessage Reply) : ZLinkActorJoinResult;

    public sealed record Rejected(ZLinkMessage Reply) : ZLinkActorJoinResult;
}
