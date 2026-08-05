namespace Zlink.Framework.Runtime.Locations;

/// <summary>
/// Owns the generated identities and descriptor claim for one MeshNode
/// lifecycle. Builder registration keeps caller intent only; generated values
/// never flow back into configuration.
/// </summary>
internal sealed record ZLinkMeshNodeStartupState(
    RoutingId RoutingId,
    string EntrySpotId,
    ZLinkMeshNodeDescriptor Descriptor,
    ulong StoreGeneration);

internal static class ZLinkMeshNodeDescriptorFactory
{
    internal static ZLinkMeshNodeDescriptor Create(
        ZLinkFrameworkRegistration framework,
        ZLinkSpotNodeRegistration registration,
        RoutingId routingId,
        ulong lifecycleGeneration,
        string endpoint,
        string entrySpotId,
        ulong descriptorRevision,
        ZLinkFrameworkRuntimeState state)
    {
        var capabilities = BuildObjectCapabilities(registration);
        return new ZLinkMeshNodeDescriptor(
            registration.SpotMeshChannelName ?? registration.SpotNodeName,
            routingId,
            lifecycleGeneration,
            descriptorRevision,
            endpoint,
            registration.ChannelMemberships
                .Where(static membership => membership.IsServer)
                .ToDictionary(
                    static membership => membership.ChannelName,
                    static membership => membership.Weight,
                    StringComparer.Ordinal),
            SecurityIdentity: ZLinkTransportSecurityIdentity.Plaintext,
            OwnerId: string.Empty,
            LeaseGeneration: 0,
            UpdatedAt: default)
        {
            ApplicationVersion = framework.ApplicationVersion,
            ObjectRole = registration.ObjectRole,
            ObjectCapabilities = capabilities,
            MaintenanceWave = framework.MaintenanceWave,
            EntrySpotId = registration.ObjectRole == ZLinkMeshNodeObjectRole.Server
                ? entrySpotId
                : null,
            State = state,
            PlacementWeight = registration.PlacementWeight,
            Capacity = new ZLinkPlacementCapacity(
                new ZLinkPopulationCapacity(0, 0, registration.ActorLimit),
                new ZLinkPopulationCapacity(0, 0, registration.SpotLimit),
                capabilities
                    .Where(static capability =>
                        capability.ObjectKind is ZLinkPlacementObjectKind.UserSpot
                            or ZLinkPlacementObjectKind.InstanceSpot)
                    .Select(static capability => new ZLinkSpotTypeCapacity(
                        capability.ObjectKind,
                        capability.StableType,
                        0,
                        0,
                        capability.Limit))
                    .ToArray()),
            ActivationConcurrency = new ZLinkActivationConcurrency(
                0,
                registration.ActivationConcurrencyLimit)
        };
    }

    private static IReadOnlyList<ZLinkObjectCapability> BuildObjectCapabilities(
        ZLinkSpotNodeRegistration registration)
    {
        var capabilities = new List<ZLinkObjectCapability>(
            registration.SpotRelocations.Count
            + registration.InstanceSpotRelocations.Count
            + registration.ActorRelocations.Count);
        AddCapabilities(
            capabilities,
            ZLinkPlacementObjectKind.UserSpot,
            registration.SpotRelocations);
        AddCapabilities(
            capabilities,
            ZLinkPlacementObjectKind.InstanceSpot,
            registration.InstanceSpotRelocations);
        AddCapabilities(
            capabilities,
            ZLinkPlacementObjectKind.Actor,
            registration.ActorRelocations);
        return capabilities
            .OrderBy(static capability => capability.ObjectKind)
            .ThenBy(static capability => capability.StableType, StringComparer.Ordinal)
            .ToArray();
    }

    private static void AddCapabilities(
        ICollection<ZLinkObjectCapability> capabilities,
        ZLinkPlacementObjectKind objectKind,
        IReadOnlyDictionary<string, ZLinkObjectRelocationRegistration> registrations)
    {
        foreach (var (stableType, registration) in registrations)
        {
            capabilities.Add(new ZLinkObjectCapability(
                objectKind,
                stableType,
                registration.PolicyKind switch
                {
                    0 => ZLinkObjectMaintenancePolicyKind.Disabled,
                    1 => ZLinkObjectMaintenancePolicyKind.Recreate,
                    2 => ZLinkObjectMaintenancePolicyKind.Snapshot,
                    _ => throw new ZLinkConfigurationException(
                        $"Unknown relocation policy kind '{registration.PolicyKind}'.")
                },
                registration.AdapterType is not null,
                objectKind == ZLinkPlacementObjectKind.Actor
                    ? 0
                    : registration.Placement.MaxActiveObjects ?? 0));
        }
    }
}
