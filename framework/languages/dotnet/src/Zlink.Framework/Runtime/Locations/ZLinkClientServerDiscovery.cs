namespace Zlink.Framework.Runtime.Locations;

internal sealed class ZLinkClientServerDiscovery : IAsyncDisposable
{
    private readonly IZLinkLocationRepository _store;
    private readonly ZLinkLocationRuntime _locationRuntime;
    private readonly ZLinkLocationOptions _options;
    private readonly ZLinkOwnerLeaseTracker? _leases;
    private readonly List<ClientLoop> _clients = [];
    private readonly List<LocalServer> _servers = [];

    internal ZLinkClientServerDiscovery(
        IZLinkLocationRepository store,
        ZLinkLocationRuntime locationRuntime,
        ZLinkLocationOptions options,
        ZLinkOwnerLeaseTracker? leases)
    {
        _store = store;
        _locationRuntime = locationRuntime;
        _options = options;
        _leases = leases;
    }

    internal async ValueTask StartAsync(
        ZLinkFrameworkComponentState state,
        CancellationToken cancellationToken)
    {
        foreach (var (channelName, registration) in state.Registration.Channels)
        {
            if (registration.HasClientServerServer
                && state.ClientServerServerBundles.TryGetValue(channelName, out var serverBundle))
            {
                var router = (IZLinkBackendRouterSocket)serverBundle.Socket;
                var server = new LocalServer(
                    serverBundle.ClientServerServer
                    ?? throw new InvalidOperationException(
                        "ClientServer server identity is not initialized."),
                    AdvertisedEndpoint(
                        router.GetLastEndpoint(),
                        registration.Server!.AdvertiseHost),
                    router);
                server.Identity.SetAdvertisedEndpoint(server.Endpoint);
                await PublishAsync(server, ZLinkLocationWriteIntent.NewClaim, cancellationToken)
                    .ConfigureAwait(false);
                _servers.Add(server);
            }

            if (registration.HasClientServerClient
                && state.ClientServerClientRuntimes.TryGetValue(
                    channelName,
                    out var clientRuntime))
            {
                var loop = new ClientLoop(
                    channelName,
                    _store,
                    clientRuntime,
                    _options,
                    _leases,
                    state.ErrorSink);
                await loop.StartAsync(cancellationToken).ConfigureAwait(false);
                _clients.Add(loop);
            }
        }
    }

    internal async ValueTask<bool> MarkDrainingAsync(
        CancellationToken cancellationToken)
    {
        var published = true;
        foreach (var server in _servers)
        {
            server.Identity.MarkDraining();
            var result = await PublishAsync(
                    server,
                    ZLinkLocationWriteIntent.Renew,
                    cancellationToken)
                .ConfigureAwait(false);
            published &= result.Status == ZLinkLocationWriteStatus.Stored;
        }

        return published;
    }

    internal ValueTask<bool> MarkRetiringAsync(CancellationToken cancellationToken) =>
        PublishStateAsync(static identity => identity.MarkRetiring(), cancellationToken);

    internal ValueTask<bool> MarkServingAsync(CancellationToken cancellationToken) =>
        PublishStateAsync(static identity => identity.MarkServing(), cancellationToken);

    private async ValueTask<bool> PublishStateAsync(
        Action<ZLinkClientServerServerIdentity> transition,
        CancellationToken cancellationToken)
    {
        var published = true;
        foreach (var server in _servers)
        {
            transition(server.Identity);
            var result = await PublishAsync(
                    server,
                    ZLinkLocationWriteIntent.Renew,
                    cancellationToken)
                .ConfigureAwait(false);
            published &= result.Status == ZLinkLocationWriteStatus.Stored;
        }
        return published;
    }

    internal void SetLocalWeight(string channelName, int weight)
    {
        ZLinkSocketConfig.ValidatePeerWeight(weight);
        var server = _servers.SingleOrDefault(candidate =>
            string.Equals(
                candidate.ChannelName,
                channelName,
                StringComparison.Ordinal));
        if (server is null)
            return;
        _ = PublishWeightAsync(server);
    }

    private async Task PublishWeightAsync(LocalServer server)
    {
        try
        {
            await PublishAsync(
                    server,
                    ZLinkLocationWriteIntent.Renew,
                    CancellationToken.None)
                .ConfigureAwait(false);
        }
        catch
        {
            // The transport revision is already active. A later lifecycle
            // publication reconciles a transient store failure.
        }
    }

    public async ValueTask DisposeAsync()
    {
        var failures = new List<Exception>();
        foreach (var client in _clients)
            try
            {
                await client.DisposeAsync().ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                failures.Add(exception);
            }
        _clients.Clear();

        foreach (var server in _servers)
            try
            {
                _ = await _store.RemoveClientServerAsync(
                        new ZLinkClientServerServerDescriptorKey(
                            server.ChannelName,
                            server.Identity.ServerRid),
                        _locationRuntime.OwnerToken,
                        CancellationToken.None)
                    .ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                failures.Add(exception);
            }

        _servers.Clear();

        if (failures.Count == 1)
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(failures[0]).Throw();
        if (failures.Count > 1)
            throw new AggregateException(failures);
    }

    private async ValueTask<ZLinkLocationWriteResult> PublishAsync(
        LocalServer server,
        ZLinkLocationWriteIntent intent,
        CancellationToken cancellationToken)
    {
        var owner = _locationRuntime.AdmissionOwnerToken;
        var snapshot = server.Identity.Read();
        var descriptor = new ZLinkClientServerServerDescriptor(
            server.ChannelName,
            server.Identity.ServerRid,
            server.Identity.LifecycleGeneration,
            snapshot.Revision,
            server.Endpoint,
            snapshot.Weight,
            snapshot.State,
            server.Identity.SecurityIdentity,
            owner.OwnerId,
            owner.LeaseGeneration,
            default);
        var result = await _store.UpdateClientServerAsync(
                descriptor,
                intent,
                cancellationToken)
            .ConfigureAwait(false);
        if (result.Status == ZLinkLocationWriteStatus.RejectedConflict
            && intent == ZLinkLocationWriteIntent.NewClaim)
            result = await _store.UpdateClientServerAsync(
                    descriptor,
                    ZLinkLocationWriteIntent.Takeover,
                    cancellationToken)
                .ConfigureAwait(false);
        if (result.Status != ZLinkLocationWriteStatus.Stored)
            throw new ZLinkConfigurationException(
                $"ClientServer server descriptor '{server.ChannelName}' could not be published.");
        return result;
    }

    private static string AdvertisedEndpoint(
        string boundEndpoint,
        string? advertiseHost)
    {
        if (string.IsNullOrWhiteSpace(advertiseHost))
            return boundEndpoint;
        var endpoint = new Uri(boundEndpoint, UriKind.Absolute);
        return new UriBuilder(endpoint) { Host = advertiseHost }.Uri
            .GetComponents(UriComponents.SchemeAndServer, UriFormat.Unescaped);
    }

    private sealed class LocalServer(
        ZLinkClientServerServerIdentity identity,
        string endpoint,
        IZLinkBackendRouterSocket router)
    {
        internal ZLinkClientServerServerIdentity Identity { get; } = identity;
        internal string ChannelName => Identity.ChannelName;
        internal string Endpoint { get; } = endpoint;
        internal IZLinkBackendRouterSocket Router { get; } = router;
    }

    private sealed class ClientLoop(
        string channelName,
        IZLinkLocationRepository store,
        ZLinkClientServerClientRuntime runtime,
        ZLinkLocationOptions options,
        ZLinkOwnerLeaseTracker? leases,
        IZLinkRuntimeFailureReporter errorSink) : IAsyncDisposable
    {
        private readonly Dictionary<string, ulong> _observedRevisions = new(StringComparer.Ordinal);
        private CancellationTokenSource? _stop;
        private Task? _loop;

        internal async ValueTask StartAsync(CancellationToken cancellationToken)
        {
            await ReconcileAsync(cancellationToken).ConfigureAwait(false);
            _stop = new CancellationTokenSource();
            _loop = Task.Run(() => RunAsync(_stop.Token), CancellationToken.None);
        }

        private async Task RunAsync(CancellationToken cancellationToken)
        {
            using var timer = new PeriodicTimer(options.PollingInterval);
            while (await timer.WaitForNextTickAsync(cancellationToken).ConfigureAwait(false))
                try
                {
                    await ReconcileAsync(cancellationToken).ConfigureAwait(false);
                }
                catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
                {
                    break;
                }
                catch (Exception exception)
                {
                    // Fail-static: retain the last successful connection set.
                    errorSink.ReportRuntimeTaskException(
                        $"client-server-discovery:{channelName}",
                        exception);
                }
        }

        private async ValueTask ReconcileAsync(CancellationToken cancellationToken)
        {
            var rows = await ListAllAsync(cancellationToken).ConfigureAwait(false);
            var desired = new Dictionary<string, Target>(StringComparer.Ordinal);
            foreach (var row in rows)
            {
                if (row.ServerRid.Size == 0
                    || row.LifecycleGeneration == 0
                    || row.DescriptorRevision == 0
                    || string.IsNullOrWhiteSpace(row.Endpoint)
                    || row.Weight is < 0 or > ZLinkSocketConfig.MaximumPeerWeight)
                    continue;
                if (leases is not null
                    && !await leases.IsOwnerTokenLiveAsync(
                            new ZLinkLocationOwnerToken(
                                row.OwnerId,
                                row.LeaseGeneration),
                            cancellationToken)
                        .ConfigureAwait(false))
                    continue;

                var lifecycleKey = $"{row.ServerRid.ToHex()}:{row.LifecycleGeneration}";
                var revisionKey = lifecycleKey;
                if (_observedRevisions.TryGetValue(revisionKey, out var observed)
                    && row.DescriptorRevision < observed)
                    continue;
                _observedRevisions[revisionKey] = row.DescriptorRevision;
                desired[lifecycleKey] = new Target(
                    lifecycleKey,
                    row.ServerRid,
                    row.LifecycleGeneration,
                    row.Endpoint,
                    row.Weight,
                    row.State == ZLinkFrameworkRuntimeState.Serving
                    && row.Weight > 0);
            }

            runtime.ReplaceAutomatic(
                rows.Where(row => desired.ContainsKey(
                        $"{row.ServerRid.ToHex()}:{row.LifecycleGeneration}"))
                    .ToArray());
        }

        private async ValueTask<IReadOnlyList<ZLinkClientServerServerDescriptor>>
            ListAllAsync(CancellationToken cancellationToken)
        {
            var result = new List<ZLinkClientServerServerDescriptor>();
            string? continuation = null;
            do
            {
                var page = await store.ListClientServersAsync(
                        channelName,
                        new ZLinkPageRequest(256, continuation),
                        cancellationToken)
                    .ConfigureAwait(false);
                result.AddRange(page.Items);
                continuation = page.ContinuationToken;
            } while (continuation is not null);
            return result;
        }

        public async ValueTask DisposeAsync()
        {
            if (_stop is not null)
            {
                await _stop.CancelAsync().ConfigureAwait(false);
                if (_loop is not null)
                    try
                    {
                        await _loop.ConfigureAwait(false);
                    }
                    catch (OperationCanceledException)
                    {
                    }
                _stop.Dispose();
                _stop = null;
                _loop = null;
            }
            runtime.ReplaceAutomatic([]);
        }

        private sealed record Target(
            string Key,
            RoutingId Rid,
            ulong LifecycleGeneration,
            string Endpoint,
            int Weight,
            bool Selectable);
    }
}
