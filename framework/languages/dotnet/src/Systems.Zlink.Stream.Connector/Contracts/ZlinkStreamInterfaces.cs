namespace Systems.Zlink.Stream.Connector.Contracts;

public interface IZlinkStreamPacketNameResolver
{
    string Resolve(Type payloadType);
}