using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.Runtime.Service;

internal enum ZLinkServiceAdmissionDecision
{
    Accept = 1,
    Idempotent,
    Reject,
}

internal enum ZLinkServiceConnectionDirection
{
    Inbound = 1,
    Outbound,
}

internal enum ZLinkServiceDuplicateConnectionDecision
{
    NotDuplicate = 1,
    KeepCurrent,
    UseIncoming,
}

internal static class ZLinkServiceAdmissionGuard
{
    private static readonly HashSet<byte> MutableExtensionFields = [1, 5, 8, 9, 10, 11, 12];

    internal static ZLinkServiceAdmissionDecision Evaluate(
        ZLinkServiceWireCodec.AdmissionRecord? current,
        ServiceWireConstants.Command command,
        ZLinkServiceWireCodec.AdmissionRecord incoming
    )
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
        ZLinkServiceWireCodec.AdmissionRecord incoming
    ) =>
        string.Equals(expectedEndpoint, incoming.AdvertisedEndpoint, StringComparison.Ordinal)
        && SecurityIdentityMatches(expectedSecurityIdentity, incoming.SecurityIdentity)
        && (
            expectedLifecycleGeneration == 0
            || expectedLifecycleGeneration == incoming.LifecycleGeneration
        );

    internal static bool MatchesExpectedTransportRoute(
        string expectedEndpoint,
        string expectedSecurityIdentity,
        string authenticatedSecurityIdentity,
        ulong expectedLifecycleGeneration,
        ZLinkServiceWireCodec.AdmissionRecord incoming
    ) =>
        MatchesExpectedRoute(
            expectedEndpoint,
            expectedSecurityIdentity,
            expectedLifecycleGeneration,
            incoming
        )
        && string.Equals(
            expectedSecurityIdentity,
            authenticatedSecurityIdentity,
            StringComparison.Ordinal
        );

    // C++ RouteMesh descriptors retain their historical unauthenticated
    // identity spelling ("default"). The actual ROUTER transport is still
    // plaintext, so accept that wire spelling only for the plaintext
    // expectation; authenticated identities remain exact-match only.
    private static bool SecurityIdentityMatches(string expected, string actual) =>
        string.Equals(expected, actual, StringComparison.Ordinal)
        || (
            string.Equals(
                expected,
                ZLinkServiceSecurityIdentity.Plaintext,
                StringComparison.Ordinal
            ) && string.Equals(actual, "default", StringComparison.Ordinal)
        );

    internal static ZLinkServiceDuplicateConnectionDecision SelectConnection(
        ulong currentLifecycleGeneration,
        ZLinkServiceConnectionDirection currentDirection,
        ulong incomingLifecycleGeneration,
        ZLinkServiceConnectionDirection incomingDirection
    )
    {
        if (
            currentLifecycleGeneration != 0
            && currentLifecycleGeneration != incomingLifecycleGeneration
        )
            return ZLinkServiceDuplicateConnectionDecision.NotDuplicate;

        // Core selects the one physical route of the RID (Core ROUTER §10.1),
        // so both objects describe the same logical peer. Keep the object
        // that owns the configured connect intent; otherwise keep the current
        // one.
        return
            incomingDirection == ZLinkServiceConnectionDirection.Outbound
            && currentDirection == ZLinkServiceConnectionDirection.Inbound
            ? ZLinkServiceDuplicateConnectionDecision.UseIncoming
            : ZLinkServiceDuplicateConnectionDecision.KeepCurrent;
    }

    private static bool ImmutableFieldsMatch(
        ZLinkServiceWireCodec.AdmissionRecord existing,
        ZLinkServiceWireCodec.AdmissionRecord incoming
    )
    {
        if (
            !string.Equals(existing.MeshName, incoming.MeshName, StringComparison.Ordinal)
            || !string.Equals(
                existing.SecurityIdentity,
                incoming.SecurityIdentity,
                StringComparison.Ordinal
            )
            || !string.Equals(
                existing.AdvertisedEndpoint,
                incoming.AdvertisedEndpoint,
                StringComparison.Ordinal
            )
            || existing.ObjectRole != incoming.ObjectRole
            || existing.ApplicationVersion != incoming.ApplicationVersion
            || existing.Channels.Count != incoming.Channels.Count
            || existing.ExtensionFields.Count != incoming.ExtensionFields.Count
        )
            return false;

        foreach (var channel in existing.Channels.Keys)
            if (!incoming.Channels.ContainsKey(channel))
                return false;

        foreach (var (id, value) in existing.ExtensionFields)
        {
            if (MutableExtensionFields.Contains(id))
                continue;
            if (
                !incoming.ExtensionFields.TryGetValue(id, out var candidate)
                || !value.AsSpan().SequenceEqual(candidate)
            )
                return false;
        }

        return incoming.ExtensionFields.Keys.All(id =>
            MutableExtensionFields.Contains(id) || existing.ExtensionFields.ContainsKey(id)
        );
    }
}
