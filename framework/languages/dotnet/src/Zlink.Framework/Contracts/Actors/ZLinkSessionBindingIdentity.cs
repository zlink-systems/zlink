namespace Zlink.Framework.Contracts.Actors;

public readonly record struct ZLinkSessionBindingIdentity(
    RoutingId SessionOwnerNodeRid,
    ulong SessionOwnerNodeGeneration,
    ulong BindingGeneration
);
