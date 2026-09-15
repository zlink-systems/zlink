using Systems.Zlink.Stream.Connector.Runtime;

namespace Systems.Zlink.Stream.Connector.Contracts
{
    public static class ZlinkStreamConnectorFactory
    {
        /// <summary>
        ///     Creates a WebGL connector over the browser bundle of
        ///     <c>@zlink-systems/stream-connector</c>. Same entry point and same return type
        ///     as the native .NET package, so game code does not branch on the build target.
        /// </summary>
        /// <exception cref="ZlinkStreamException">
        ///     The options are rejected, for example a <c>tcp://</c> or <c>tls://</c> endpoint
        ///     that the browser sandbox cannot open.
        /// </exception>
        public static IZlinkStreamConnector Create(ZlinkStreamConnectorOptions options)
        {
            return new ZlinkStreamWebGlConnector(options);
        }
    }
}
