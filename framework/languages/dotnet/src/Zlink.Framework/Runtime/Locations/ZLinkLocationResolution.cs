namespace Zlink.Framework.Runtime.Locations;

/// <summary>
/// Describes why a logical object did not produce a usable direct route.
/// A durable authority record can remain after its owner process stops, so a
/// route that cannot be used is not the same as an object that never existed
/// or was explicitly released.
/// </summary>
internal enum ZLinkLocationResolutionKind
{
    Missing,
    Ready,
    KnownUnavailable
}
