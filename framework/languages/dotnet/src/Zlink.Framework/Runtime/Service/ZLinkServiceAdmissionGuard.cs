using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.Runtime.Service;

internal enum ZLinkServiceAdmissionDecision
{
    Accept = 1,
    Idempotent,
    Reject
}

internal enum ZLinkServiceConnectionDirection
{
    Inbound = 1,
    Outbound
}

internal enum ZLinkServiceDuplicateConnectionDecision
{
    NotDuplicate = 1,
    KeepCurrent,
    UseIncoming
}

internal static class ZLinkServiceAdmissionGuard
{
    private static readonly HashSet<byte> MutableExtensionFields =
        [1, 5, 8, 9, 10, 11, 12];

    internal static ZLinkServiceAdmissionDecision Evaluate(
        ZLinkServiceWireCodec.AdmissionRecord? current,
        ServiceWireConstants.Command command,
        ZLinkServiceWireCodec.AdmissionRecord incoming)
    {
        if (current is null)
            return command == ServiceWireConstants.Command.Update
                ? ZLinkServiceAdmissionDecision.Reject
                : ZLinkServiceAdmissionDecision.Accept;

        var existing = current.Value;
        if (incoming.LifecycleGeneration != existing.LifecycleGeneration)
            return command == ServiceWireConstants.Command.Update
                ? ZLinkServiceAdmissionDecision.Reject
                : ZLinkServiceAdmissionDecision.Accept;

        if (incoming.DescriptorRevision < existing.DescriptorRevision)
            return ZLinkServiceAdmissionDecision.Reject;
        if (incoming.DescriptorRevision == existing.DescriptorRevision)
            return incoming.DescriptorBytes.AsSpan().SequenceEqual(existing.DescriptorBytes)
                ? ZLinkServiceAdmissionDecision.Idempotent
                : ZLinkServiceAdmissionDecision.Reject;
        if (command != ServiceWireConstants.Command.Update)
            return ZLinkServiceAdmissionDecision.Reject;

        return ImmutableFieldsMatch(existing, incoming)
            ? ZLinkServiceAdmissionDecision.Accept
            : ZLinkServiceAdmissionDecision.Reject;
    }

    internal static bool MatchesExpectedRoute(
        string expectedEndpoint,
        string expectedSecurityIdentity,
        ulong expectedLifecycleGeneration,
        ZLinkServiceWireCodec.AdmissionRecord incoming) =>
        string.Equals(
            expectedEndpoint,
            incoming.AdvertisedEndpoint,
            StringComparison.Ordinal)
        && string.Equals(
            expectedSecurityIdentity,
            incoming.SecurityIdentity,
            StringComparison.Ordinal)
        && (expectedLifecycleGeneration == 0
            || expectedLifecycleGeneration
                == incoming.LifecycleGeneration);

    internal static bool MatchesExpectedTransportRoute(
        string expectedEndpoint,
        string expectedSecurityIdentity,
        string authenticatedSecurityIdentity,
        ulong expectedLifecycleGeneration,
        ZLinkServiceWireCodec.AdmissionRecord incoming) =>
        MatchesExpectedRoute(
            expectedEndpoint,
            expectedSecurityIdentity,
            expectedLifecycleGeneration,
            incoming)
        && string.Equals(
            expectedSecurityIdentity,
            authenticatedSecurityIdentity,
            StringComparison.Ordinal);

    internal static ZLinkServiceDuplicateConnectionDecision SelectConnection(
        RoutingId localRid,
        RoutingId peerRid,
        ulong currentLifecycleGeneration,
        ZLinkServiceConnectionDirection currentDirection,
        string currentDiscriminator,
        ulong incomingLifecycleGeneration,
        ZLinkServiceConnectionDirection incomingDirection,
        string incomingDiscriminator)
    {
        if (currentLifecycleGeneration != 0
            && currentLifecycleGeneration != incomingLifecycleGeneration)
            return ZLinkServiceDuplicateConnectionDecision.NotDuplicate;

        // Both peers derive the same surviving pipe: the lower RID owns the
        // outbound side. Same-direction candidates use their stable
        // connection-local discriminator as the final tie-breaker.
        var preferredDirection = string.CompareOrdinal(
                localRid.ToHex(),
                peerRid.ToHex()) < 0
            ? ZLinkServiceConnectionDirection.Outbound
            : ZLinkServiceConnectionDirection.Inbound;
        if (currentDirection != incomingDirection)
            return currentDirection == preferredDirection
                ? ZLinkServiceDuplicateConnectionDecision.KeepCurrent
                : ZLinkServiceDuplicateConnectionDecision.UseIncoming;

        return StringComparer.Ordinal.Compare(
                currentDiscriminator,
                incomingDiscriminator) <= 0
            ? ZLinkServiceDuplicateConnectionDecision.KeepCurrent
            : ZLinkServiceDuplicateConnectionDecision.UseIncoming;
    }

    private static bool ImmutableFieldsMatch(
        ZLinkServiceWireCodec.AdmissionRecord existing,
        ZLinkServiceWireCodec.AdmissionRecord incoming)
    {
        if (!string.Equals(existing.MeshName, incoming.MeshName, StringComparison.Ordinal)
            || !string.Equals(
                existing.SecurityIdentity,
                incoming.SecurityIdentity,
                StringComparison.Ordinal)
            || existing.EffectiveMaxMessageBytes != incoming.EffectiveMaxMessageBytes
            || !string.Equals(
                existing.AdvertisedEndpoint,
                incoming.AdvertisedEndpoint,
                StringComparison.Ordinal)
            || existing.ObjectRole != incoming.ObjectRole
            || existing.ApplicationVersion != incoming.ApplicationVersion
            || existing.Channels.Count != incoming.Channels.Count
            || existing.ExtensionFields.Count != incoming.ExtensionFields.Count)
            return false;

        foreach (var channel in existing.Channels.Keys)
            if (!incoming.Channels.ContainsKey(channel))
                return false;

        foreach (var (id, value) in existing.ExtensionFields)
        {
            if (MutableExtensionFields.Contains(id))
                continue;
            if (!incoming.ExtensionFields.TryGetValue(id, out var candidate)
                || !value.AsSpan().SequenceEqual(candidate))
                return false;
        }

        return incoming.ExtensionFields.Keys.All(
            id => MutableExtensionFields.Contains(id)
                  || existing.ExtensionFields.ContainsKey(id));
    }
}
