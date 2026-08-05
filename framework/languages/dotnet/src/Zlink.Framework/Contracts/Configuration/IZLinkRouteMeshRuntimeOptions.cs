namespace Zlink.Framework.Contracts.Configuration;

/// <summary>
/// Public DI singleton for RouteMesh placement and local Server weight changes.
/// Looking up an unregistered mesh or ChannelName throws
/// <see cref="ZLinkConfigurationException"/>.
/// </summary>
public interface IZLinkRouteMeshRuntimeOptions
{
    IZLinkMeshPlacementRuntimeOptions Mesh(string meshName);

    IZLinkMeshChannelRuntimeOptions Channel(string channelName);
}

/// <summary>
/// Runtime node placement weight. Zero excludes the node from new placement
/// without changing existing objects or completed reservations.
/// </summary>
public interface IZLinkMeshPlacementRuntimeOptions
{
    int PlacementWeight { get; set; }
}

/// <summary>
/// Runtime RouteMesh or ClientServer Server weight. Values are signed integers
/// in the 0..10000 range. Zero excludes the membership from new selections.
/// </summary>
public interface IZLinkMeshChannelRuntimeOptions
{
    int Weight { get; set; }
}
