using System;

namespace Systems.Zlink.Stream.Connector.Contracts
{
    [AttributeUsage(AttributeTargets.Class | AttributeTargets.Struct)]
    public sealed class ZlinkStreamPacketNameAttribute : Attribute
    {
        public ZlinkStreamPacketNameAttribute(string name)
        {
            Name = name;
        }

        public string Name { get; }
    }
}
