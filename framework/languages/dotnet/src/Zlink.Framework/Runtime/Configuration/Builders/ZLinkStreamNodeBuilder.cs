namespace Zlink.Framework.Runtime.Configuration.Builders;

internal sealed class ZLinkStreamNodeBuilder(ZLinkStreamNodeRegistration registration) : IZLinkStreamNodeBuilder
{
    public IZLinkStreamNodeBuilder Bind(string endpoint)
    {
        if (string.IsNullOrWhiteSpace(endpoint))
            throw new ZLinkConfigurationException("STREAM bind endpoint must not be empty.");

        registration.BindEndpoint = endpoint;
        registration.ListenPort = null;
        return this;
    }

    public IZLinkStreamNodeBuilder Bind(int port = 0)
    {
        if (port is < 0 or > 65535)
            throw new ZLinkConfigurationException(
                "STREAM bind port must be between 0 and 65535.");
        registration.ListenPort = port;
        registration.BindEndpoint = null;
        return this;
    }

    public IZLinkStreamNodeBuilder SetBindHost(string bindHost)
    {
        registration.BindHost = ZLinkChannelEndpointBuilderSupport.Validate(
            bindHost,
            "STREAM bind host must not be empty.");
        return this;
    }

    public IZLinkStreamNodeBuilder SetAdvertiseHost(string advertiseHost)
    {
        registration.AdvertiseHost = ZLinkChannelEndpointBuilderSupport.Validate(
            advertiseHost,
            "STREAM advertise host must not be empty.");
        return this;
    }

    public IZLinkSocketConfig ConfigureSocket() => registration.SocketConfig;

    public IZLinkStreamNodeBuilder EnableActorDispatch()
    {
        if (registration.ActorDispatchEnabled)
            throw new ZLinkConfigurationException(
                $"STREAM node '{registration.StreamNodeName}' already enabled actor dispatch.");

        registration.ActorDispatchEnabled = true;
        return this;
    }

    public IZLinkStreamNodeBuilder SetTlsServer(
        string certificatePath,
        string keyPath,
        bool requireClientCertificate = false)
    {
        if (string.IsNullOrWhiteSpace(certificatePath))
            throw new ZLinkConfigurationException("STREAM TLS certificate path must not be empty.");

        if (string.IsNullOrWhiteSpace(keyPath))
            throw new ZLinkConfigurationException("STREAM TLS key path must not be empty.");

        registration.TlsServer = new ZLinkStreamTlsServerRegistration(
            certificatePath,
            keyPath,
            requireClientCertificate);
        return this;
    }

    public IZLinkStreamNodeBuilder AddSession<TSession>()
        where TSession : class, IZLinkSession
    {
        if (registration.HeaderSessionType is not null)
            throw new ZLinkConfigurationException(
                $"STREAM node '{registration.StreamNodeName}' already has a stream session.");

        registration.HeaderSessionType = typeof(TSession);
        return this;
    }
}
