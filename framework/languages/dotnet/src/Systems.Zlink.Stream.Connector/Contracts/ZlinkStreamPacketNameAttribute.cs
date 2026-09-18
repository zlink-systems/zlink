namespace Systems.Zlink.Stream.Connector.Contracts;

/// <summary>
///     Fixes the packet name of a payload type (stream-connector spec §5). The name on the
///     type wins over the simple type name; a name given on the operation wins over both.
/// </summary>
/// <remarks>
///     <see cref="AttributeUsageAttribute.Inherited" /> stays at its default of
///     <see langword="true" />, and <c>ZlinkStreamPacketNameResolver</c> reads it that way,
///     so a derived payload type carries the packet name of its base unless it declares
///     one of its own.
/// </remarks>
[AttributeUsage(AttributeTargets.Class | AttributeTargets.Struct, Inherited = true)]
public sealed class ZlinkStreamPacketNameAttribute(string name) : Attribute
{
    /// <summary>Packet name this type sends and receives under.</summary>
    public string Name { get; } = name;
}