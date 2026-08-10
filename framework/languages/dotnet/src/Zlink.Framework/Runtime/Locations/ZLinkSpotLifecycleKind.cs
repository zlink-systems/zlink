using Zlink.Framework.Runtime.Identifiers;

namespace Zlink.Framework.Runtime.Locations;

// Wire/storage tags are converted once at the location boundary. Domain
// lifecycle code then receives one of these closed variants, so Entry cannot
// accidentally enter User/Instance relocation rules.
internal abstract record ZLinkSpotLifecycleKind
{
    private ZLinkSpotLifecycleKind()
    {
    }

    internal abstract ZLinkPlacementObjectKind? PlacementKind { get; }

    internal abstract bool MatchesReady(
        ZLinkAuthoritySnapshot snapshot,
        ZLinkMeshName meshName,
        ZLinkSpotId spotId,
        string? stableType,
        RoutingId nodeRid,
        ulong nodeGeneration);

    internal abstract bool MatchesTracked(
        ZLinkAuthoritySnapshot snapshot,
        ZLinkSpotId spotId,
        ZLinkMeshName meshName,
        string? stableType,
        RoutingId nodeRid,
        ulong nodeGeneration);

    internal static ZLinkSpotLifecycleKind FromBoundary(ZLinkSpotKind kind) =>
        kind switch
        {
            ZLinkSpotKind.Entry => Entry.Value,
            ZLinkSpotKind.User => User.Value,
            ZLinkSpotKind.Instance => Instance.Value,
            _ => throw new ArgumentOutOfRangeException(nameof(kind))
        };

    internal static ZLinkSpotLifecycleKind RelocatableFromBoundary(
        ZLinkSpotKind kind)
    {
        var mapped = FromBoundary(kind);
        return mapped is Entry
            ? throw new InvalidOperationException(
                "An Entry Spot cannot enter the relocatable Spot lifecycle.")
            : mapped;
    }

    internal sealed record Entry : ZLinkSpotLifecycleKind
    {
        internal static Entry Value { get; } = new();

        private Entry()
        {
        }

        internal override ZLinkPlacementObjectKind? PlacementKind => null;

        internal override bool MatchesReady(
            ZLinkAuthoritySnapshot snapshot,
            ZLinkMeshName meshName,
            ZLinkSpotId spotId,
            string? stableType,
            RoutingId nodeRid,
            ulong nodeGeneration) => false;

        internal override bool MatchesTracked(
            ZLinkAuthoritySnapshot snapshot,
            ZLinkSpotId spotId,
            ZLinkMeshName meshName,
            string? stableType,
            RoutingId nodeRid,
            ulong nodeGeneration) => false;
    }

    internal sealed record User : ZLinkSpotLifecycleKind
    {
        internal static User Value { get; } = new();

        private User()
        {
        }

        internal override ZLinkPlacementObjectKind? PlacementKind =>
            ZLinkPlacementObjectKind.UserSpot;

        internal override bool MatchesReady(
            ZLinkAuthoritySnapshot snapshot,
            ZLinkMeshName meshName,
            ZLinkSpotId spotId,
            string? stableType,
            RoutingId nodeRid,
            ulong nodeGeneration) =>
            ZLinkUserSpotAuthorityPayloadCodec.TryDecode(
                snapshot.Payload.Span,
                out var user)
            && user.State == ZLinkUserSpotAuthorityState.Ready
            && user.SpotId == spotId.Value
            && user.MeshName == meshName.Value
            && user.NodeRid == nodeRid
            && user.NodeGeneration == nodeGeneration
            && (stableType is null || user.StableType == stableType);

        internal override bool MatchesTracked(
            ZLinkAuthoritySnapshot snapshot,
            ZLinkSpotId spotId,
            ZLinkMeshName meshName,
            string? stableType,
            RoutingId nodeRid,
            ulong nodeGeneration) =>
            ZLinkUserSpotAuthorityPayloadCodec.TryDecode(
                snapshot.Payload.Span,
                out var user)
            && user.SpotId == spotId.Value
            && user.MeshName == meshName.Value
            && user.StableType == stableType
            && user.NodeRid == nodeRid
            && user.NodeGeneration == nodeGeneration;
    }

    internal sealed record Instance : ZLinkSpotLifecycleKind
    {
        internal static Instance Value { get; } = new();

        private Instance()
        {
        }

        internal override ZLinkPlacementObjectKind? PlacementKind =>
            ZLinkPlacementObjectKind.InstanceSpot;

        internal override bool MatchesReady(
            ZLinkAuthoritySnapshot snapshot,
            ZLinkMeshName meshName,
            ZLinkSpotId spotId,
            string? stableType,
            RoutingId nodeRid,
            ulong nodeGeneration) =>
            ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
                snapshot.Payload.Span,
                out var instance)
            && instance.State == ZLinkInstanceSpotAuthorityState.Ready
            && instance.SpotId == spotId.Value
            && instance.MeshName == meshName.Value
            && instance.NodeRid == nodeRid
            && instance.NodeGeneration == nodeGeneration
            && (stableType is null || instance.StableType == stableType);

        internal override bool MatchesTracked(
            ZLinkAuthoritySnapshot snapshot,
            ZLinkSpotId spotId,
            ZLinkMeshName meshName,
            string? stableType,
            RoutingId nodeRid,
            ulong nodeGeneration) =>
            ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
                snapshot.Payload.Span,
                out var instance)
            && instance.SpotId == spotId.Value
            && instance.MeshName == meshName.Value
            && instance.StableType == stableType
            && instance.NodeRid == nodeRid
            && instance.NodeGeneration == nodeGeneration;
    }
}
