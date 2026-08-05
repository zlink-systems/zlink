namespace Systems.Zlink.Stream.Connector.Contracts;

[AttributeUsage(AttributeTargets.Class | AttributeTargets.Struct)]
public sealed class ZlinkStreamPacketNameAttribute(string name) : Attribute
{
    public string Name { get; } = name;
}