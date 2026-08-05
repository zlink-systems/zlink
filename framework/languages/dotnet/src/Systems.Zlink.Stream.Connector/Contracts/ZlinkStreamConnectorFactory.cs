namespace Systems.Zlink.Stream.Connector.Contracts;

public static class ZlinkStreamConnectorFactory
{
    public static IZlinkStreamConnector Create(ZlinkStreamConnectorOptions options)
    {
        return new ZlinkStreamConnector(options);
    }
}