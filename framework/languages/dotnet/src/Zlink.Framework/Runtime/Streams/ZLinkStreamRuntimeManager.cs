namespace Zlink.Framework.Runtime.Streams;

internal sealed class ZLinkStreamRuntimeManager(
    IServiceProvider services,
    IZLinkBackendAdapterFactory backendAdapterFactory,
    ZLinkFrameworkRegistration registration
)
{
    /// <summary>
    ///     Creates each STREAM node. The node is recorded in <paramref name="state" /> as soon
    ///     as its socket exists and receives the monitor and endpoints as they are created, so
    ///     the state's startup rollback and shutdown dispose a partly started node the same
    ///     way as a running one. Until the node exists, this method owns the socket and
    ///     disposes it when creating the node fails.
    /// </summary>
    public async ValueTask InitializeStreamNodesAsync(ZLinkFrameworkComponentState state)
    {
        if (registration.StreamNodes.Count == 0)
            return;

        var monitoringAdapter = backendAdapterFactory.CreateMonitoringAdapter();

        foreach (var streamNodeRegistration in registration.StreamNodes.Values)
        {
            var socket = state.Context.CreateStreamSocket(
                streamNodeRegistration.StreamNodeName,
                actorDispatchNode: null
            );
            ZLinkStreamNodeRuntime runtime;
            try
            {
                runtime = new ZLinkStreamNodeRuntime(
                    streamNodeRegistration.StreamNodeName,
                    services,
                    socket,
                    monitor: null,
                    streamNodeRegistration.HeaderSessionType,
                    state.TaskRunner,
                    streamNodeRegistration.TlsServer is null ? "tcp" : "tls",
                    actorDispatchEnabled: streamNodeRegistration.ActorDispatchEnabled,
                    maxMessageSize: streamNodeRegistration.SocketConfig.MaxMessageSize,
                    applicationJobQueue: state.ApplicationJobQueue
                );
            }
            catch
            {
                await socket.DisposeAsync().ConfigureAwait(false);
                throw;
            }
            state.StreamNodes.Add(streamNodeRegistration.StreamNodeName, runtime);

            if (streamNodeRegistration.TlsServer is { } tlsServer)
                socket.SetTlsServer(
                    tlsServer.CertPath,
                    tlsServer.KeyPath,
                    tlsServer.RequireClientCert
                );

            socket.ApplySocketConfig(streamNodeRegistration.SocketConfig);
            var bindEndpoint = ZLinkNetworkEndpointResolver.Bind(
                streamNodeRegistration.BindEndpoint,
                streamNodeRegistration.ListenPort,
                streamNodeRegistration.BindHost,
                registration.NetworkOptions
            );
            socket.Bind(bindEndpoint);
            var boundEndpoint = socket.GetLastEndpoint();
            if (string.IsNullOrWhiteSpace(boundEndpoint))
            {
                if (streamNodeRegistration.ListenPort == 0)
                    throw new ZLinkConfigurationException(
                        $"STREAM node '{streamNodeRegistration.StreamNodeName}' did not report the endpoint selected for port 0."
                    );
                boundEndpoint = bindEndpoint;
            }
            runtime.SetEndpoints(
                boundEndpoint,
                ZLinkNetworkEndpointResolver.Advertise(
                    boundEndpoint,
                    streamNodeRegistration.AdvertiseHost,
                    streamNodeRegistration.BindHost,
                    registration.NetworkOptions
                )
            );
            runtime.AttachMonitor(monitoringAdapter.OpenSocketMonitor(socket));
            runtime.Start();
            await state.ListenerRecords.RecordAsync(
                ZLinkListenerKind.Stream,
                streamNodeRegistration.StreamNodeName,
                runtime.AdvertisedEndpoint!
            ).ConfigureAwait(false);
        }
    }
}
