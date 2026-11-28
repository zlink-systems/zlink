using Zlink.Framework.Runtime.Configuration.Builders;
using Zlink.Framework.Runtime.Spots;
using Zlink.Framework.Runtime.Streams;

namespace Zlink.Framework.Runtime.Configuration;

internal sealed record ZLinkAdmissionQueueConfiguration(
    string MeshName,
    int? GlobalLimit,
    ZLinkAdmissionTicket Ticket
)
{
    internal static ZLinkAdmissionQueueConfiguration? Configure(
        ZLinkFrameworkRegistration registration
    )
    {
        var streams = registration.StreamNodes.Values;
        var first = streams.FirstOrDefault();
        if (first is null)
            return null;
        var options = first.AdmissionQueue;
        foreach (var stream in streams)
        {
            var next = stream.AdmissionQueue;
            if (options.Enabled != next.Enabled || (options.Enabled && options != next))
                throw new ZLinkConfigurationException(
                    "STREAM registrations on the same host must use the same admission queue settings."
                );
            if (options.Enabled && !ZLinkAdmissionTicket.IsStreamName(stream.StreamNodeName))
                throw new ZLinkConfigurationException(
                    "An admission queue STREAM registration name must encode to 1 through 255 UTF-8 bytes."
                );
        }

        if (!options.Enabled)
            return null;
        if (first.AuthenticationAdmission.PacketName is null)
            throw new ZLinkConfigurationException(
                "AdmissionQueueEnabled requires AuthenticationPacketName."
            );
        if (options.GlobalLimit is <= 0)
            throw new ZLinkConfigurationException("AdmissionGlobalLimit must be positive.");

        var ticket = new ZLinkAdmissionTicket(options.Key, options.PreviousKey);
        var meshes = registration.SpotNodes;
        if (meshes.Count == 0)
            throw new ZLinkConfigurationException("The admission queue requires a RouteMesh.");
        var meshName = options.MeshName;
        if (meshName is null)
        {
            if (meshes.Count != 1)
                throw new ZLinkConfigurationException("Mesh selection is required");
            meshName = meshes.Keys.Single();
        }
        if (
            !meshes.TryGetValue(meshName, out var mesh)
            || mesh.ObjectRole == ZLinkMeshNodeObjectRole.None
            || registration.Locations.ResolveStore() is null
        )
            throw new ZLinkConfigurationException(
                "The admission queue mesh requires a Location Store and an Object Client or Server role."
            );
        if (!ZLinkSpotId.IsValid(AdmissionQueueMessages.SpotId(meshName)))
            throw new ZLinkConfigurationException(
                "The admission queue Spot ID must encode to 1 through 255 UTF-8 bytes."
            );
        if (mesh.ObjectRole == ZLinkMeshNodeObjectRole.Server)
            new ZLinkMeshNodeBuilder(mesh).RegisterInstanceSpotFactory<ZLinkAdmissionQueueSpot>(
                AdmissionQueueMessages.SpotType,
                static factory => factory.RecreateOnRelocation()
            );

        return new ZLinkAdmissionQueueConfiguration(meshName, options.GlobalLimit, ticket);
    }
}
