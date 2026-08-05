namespace Zlink.Framework.Runtime.Streams;

internal sealed class ZLinkStreamRuntimeManager(
    IServiceProvider services,
    IZLinkBackendAdapterFactory backendAdapterFactory,
    ZLinkFrameworkRegistration registration)
{
    public async ValueTask InitializeStreamNodesAsync(ZLinkFrameworkComponentState state)
    {
        if (registration.StreamNodes.Count == 0) return;

        var streamAdapter = backendAdapterFactory.CreateStreamAdapter();
        var monitoringAdapter = backendAdapterFactory.CreateMonitoringAdapter();

        foreach (var streamNodeRegistration in registration.StreamNodes.Values)
        {
            IZLinkBackendStreamSocket? socket = null;
            IZLinkBackendSocketMonitor? monitor = null;
            ZLinkStreamNodeRuntime? runtime = null;
            try
            {
                socket = streamAdapter.CreateStreamSocket(
                    state.Context,
                    streamNodeRegistration.StreamNodeName,
                    actorDispatchNode: null);
                if (streamNodeRegistration.TlsServer is { } tlsServer)
                    socket.SetTlsServer(tlsServer.CertPath, tlsServer.KeyPath, tlsServer.RequireClientCert);

                socket.ApplySocketConfig(streamNodeRegistration.SocketConfig);
                var bindEndpoint = ZLinkNetworkEndpointResolver.Bind(
                    streamNodeRegistration.BindEndpoint,
                    streamNodeRegistration.ListenPort,
                    streamNodeRegistration.BindHost,
                    registration.NetworkOptions);
                socket.Bind(bindEndpoint);
                var boundEndpoint = socket.GetLastEndpoint();
                if (string.IsNullOrWhiteSpace(boundEndpoint))
                {
                    if (streamNodeRegistration.ListenPort == 0)
                        throw new ZLinkConfigurationException(
                            $"STREAM node '{streamNodeRegistration.StreamNodeName}' did not report the endpoint selected for port 0.");
                    boundEndpoint = bindEndpoint;
                }
                var advertisedEndpoint = ZLinkNetworkEndpointResolver.Advertise(
                    boundEndpoint,
                    streamNodeRegistration.AdvertiseHost,
                    streamNodeRegistration.BindHost,
                    registration.NetworkOptions);
                monitor = monitoringAdapter.OpenSocketMonitor(socket);

                runtime = new ZLinkStreamNodeRuntime(
                    streamNodeRegistration.StreamNodeName,
                    services,
                    socket,
                    monitor,
                    streamNodeRegistration.HeaderSessionType,
                    state.TaskRunner,
                    streamNodeRegistration.TlsServer is null ? "tcp" : "tls",
                    actorDispatchEnabled: streamNodeRegistration.ActorDispatchEnabled,
                    boundEndpoint: boundEndpoint,
                    advertisedEndpoint: advertisedEndpoint,
                    inboundDispatchBudget: state.InboundDispatchBudget,
                    completionAdmission: state.CompletionAdmission,
                    maxMessageSize: streamNodeRegistration.SocketConfig.MaxMessageSize);
                state.StreamNodes.Add(streamNodeRegistration.StreamNodeName, runtime);
                runtime.Start();
            }
            catch (Exception initializationFailure)
            {
                state.StreamNodes.Remove(streamNodeRegistration.StreamNodeName);
                var failures = new ZLinkFailureCollector(initializationFailure);
                if (runtime is not null)
                {
                    await failures.CaptureAsync(runtime.DisposeAsync).ConfigureAwait(false);
                }
                else
                {
                    if (monitor is not null)
                        await failures.CaptureAsync(monitor.DisposeAsync).ConfigureAwait(false);

                    if (socket is not null)
                        await failures.CaptureAsync(socket.DisposeAsync).ConfigureAwait(false);
                }

                failures.ThrowIfAny();
                throw new InvalidOperationException("Unreachable after startup cleanup failure propagation.");
            }
        }
    }

}
