namespace Zlink.Framework.Contracts.Configuration;

internal static class ZLinkTopologyStateExtensions
{
    internal static bool IsTerminal(this ZLinkTopologyState state) =>
        state is ZLinkTopologyState.Stopped or ZLinkTopologyState.Failed;
}

internal sealed record ZLinkLocationRuntimeSnapshot(
    string State,
    DateTimeOffset? LastSuccessAt,
    DateTimeOffset? LastFailureAt
)
{
    internal const string ReadyState = "ready";
    internal const string HealthyState = "healthy";
    internal const string DegradedState = "degraded";
    internal const string NotConfiguredState = "not_configured";
    internal const string UnknownState = "unknown";
}

internal sealed record ZLinkInstanceSpotTypeSnapshot(
    string InstanceSpotType,
    ulong ActiveCount,
    ulong ActivatingCount,
    ulong ClosingCount,
    ulong PendingMessageCount,
    ulong PendingByteCount,
    string? LastActivationOutcome
);
