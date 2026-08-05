namespace Zlink.Framework.Runtime.Configuration;

internal static partial class ZLinkFrameworkRegistrationValidator
{
    private static void ValidateSpotNode(
        ZLinkSpotNodeRegistration spotNode,
        ZLinkFrameworkRegistration registration,
        ISet<Type> globalSpotFactories,
        ISet<Type> globalEntrySpots,
        ZLinkHandlerExposureCatalog handlerExposure)
    {
        if (spotNode.Router is not null
            && string.IsNullOrWhiteSpace(spotNode.Router.BindEndpoint)
            && spotNode.Router.ListenPort is null)
            throw new ZLinkConfigurationException(
                $"SPOT node '{spotNode.SpotNodeName}' enables router capability but does not define a router bind endpoint.");

        if (spotNode.Router is null)
            throw new ZLinkConfigurationException(
                $"MeshNode '{spotNode.SpotNodeName}' must define its ROUTER endpoint via Listen(...).");

        if (spotNode.Router is null && spotNode.ActorFactories.Count > 0)
            throw new ZLinkConfigurationException(
                $"MeshNode '{spotNode.SpotNodeName}' requires a ROUTER endpoint when actor factories are registered.");

        // S8-04B fail-fast (spec 05-route-mesh §7): distributed Spot/Actor
        // addressing and Actor transfer resolve peers through a location store.
        // A production host that registers Spots, Actors, an Entry Spot or a
        // transfer adapter without any store fails startup here; single-process
        // contract tests opt in with the in-memory store (Locations.Enabled).
        if (!registration.Locations.Enabled && SpotNodeUsesDistributedLookup(spotNode))
            throw new ZLinkConfigurationException(
                $"MeshNode '{spotNode.SpotNodeName}' uses distributed Spot/Actor addressing or Actor transfer "
                + "but no location store is registered. Register the production store via AddLocationStore(...) "
                + "(spec 05-route-mesh §7).");

        ValidateObjectClientBoundary(spotNode);

        if (spotNode.Router is { } router)
        {
            router.AcquisitionMode = ZLinkPeerAcquisitionPolicy.Resolve(
                registration.Locations.Enabled,
                router.ManualConnections);
            router.ManualConnections.Freeze(router.AcquisitionMode);

            var automatic =
                router.AcquisitionMode == ZLinkPeerAcquisitionMode.AutoConnect;
            if (spotNode.HasExplicitRoutingId
                && (automatic || spotNode.ObjectRoleSelected))
                throw new ZLinkConfigurationException(
                    $"MeshNode '{spotNode.SpotNodeName}' cannot use a fixed routing ID "
                    + "with automatic discovery or an Object role.");
            if (spotNode.RoutingIdPrefix is not null && !automatic)
                throw new ZLinkConfigurationException(
                    $"MeshNode '{spotNode.SpotNodeName}' can use a routing ID prefix "
                    + "only with automatic discovery.");
            if (spotNode.ObjectRoleSelected && !registration.Locations.Enabled)
                throw new ZLinkConfigurationException(
                    $"MeshNode '{spotNode.SpotNodeName}' requires a Location Store "
                    + "when an Object role is configured.");
        }
        ValidateUniqueSpotFactories(spotNode, globalSpotFactories);
        ValidateUniqueEntrySpot(spotNode, globalEntrySpots);
        handlerExposure.ValidateMeshNode(spotNode);
    }

    private static void ValidateObjectClientBoundary(
        ZLinkSpotNodeRegistration spotNode)
    {
        if (spotNode.ObjectRole != ZLinkMeshNodeObjectRole.Client)
            return;
        if (spotNode.RouteSendHandlers.Count != 0
            || spotNode.RouteRequestHandlers.Count != 0)
            throw new ZLinkConfigurationException(
                $"MeshNode '{spotNode.SpotNodeName}' is an Object Client and cannot "
                + "register application Node-direct handlers.");
    }

    private static bool SpotNodeUsesDistributedLookup(ZLinkSpotNodeRegistration spotNode)
    {
        return spotNode.SpotFactories.Count > 0
               || spotNode.InstanceSpotFactories.Count > 0
               || spotNode.EntrySpotType is not null
               || spotNode.ActorFactories.Count > 0;
    }

    private static void ValidateUniqueSpotFactories(
        ZLinkSpotNodeRegistration spotNode,
        ISet<Type> globalSpotFactories)
    {
        foreach (var spotFactory in spotNode.SpotFactories)
            if (!globalSpotFactories.Add(spotFactory))
                throw new ZLinkConfigurationException(
                    $"Duplicate SPOT factory '{spotFactory}' across nodes.");

        foreach (var registration in spotNode.InstanceSpotFactories.Values)
        {
            if (spotNode.SpotFactories.Contains(registration.SpotType))
                throw new ZLinkConfigurationException(
                    $"Spot type '{registration.SpotType}' cannot be registered "
                    + "as both Domain and Instance Spot on one MeshNode.");
            if (!globalSpotFactories.Add(registration.SpotType))
                throw new ZLinkConfigurationException(
                    $"Duplicate SPOT factory '{registration.SpotType}' across nodes.");
        }
    }

    private static void ValidateUniqueEntrySpot(
        ZLinkSpotNodeRegistration spotNode,
        ISet<Type> globalEntrySpots)
    {
        if (spotNode.EntrySpotType is { } entrySpotType
            && !globalEntrySpots.Add(entrySpotType))
            throw new ZLinkConfigurationException(
                $"Duplicate Entry Spot '{entrySpotType}' across nodes.");
    }
}
