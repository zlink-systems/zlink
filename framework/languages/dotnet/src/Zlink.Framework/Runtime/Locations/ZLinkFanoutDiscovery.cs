namespace Zlink.Framework.Runtime.Locations;

/// <summary>
/// Owns the classic fanout discovery aggregate. Publisher descriptor
/// publication and automatic subscriber reconciliation stay separate from
/// MeshNode and ClientServer descriptor domains.
/// </summary>
internal sealed class ZLinkFanoutDiscovery : IAsyncDisposable
{
    private readonly IZLinkLocationRepository _store;
    private readonly ZLinkLocationRuntime _locationRuntime;
    private readonly ZLinkLocationOptions _options;
    private readonly ZLinkOwnerLeaseTracker? _leases;
    private readonly List<Publisher> _publishers = [];
    private readonly List<SubscriberLoop> _subscribers = [];

    internal ZLinkFanoutDiscovery(
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
            if (registration.AutoConnectType != ZLinkLocationAutoConnectType.Fanout)
                continue;

            if (registration.Publisher is not null
                && state.PublisherBundles.TryGetValue(channelName, out var publisherBundle)
                && publisherBundle.FanoutPublisher is { } identity)
            {
                var publisher = new Publisher(identity);
                await PublishAsync(
                        publisher,
                        ZLinkLocationWriteIntent.NewClaim,
                        cancellationToken)
                    .ConfigureAwait(false);
                _publishers.Add(publisher);
            }

            if (registration.Subscriber?.AcquisitionMode
                    != ZLinkPeerAcquisitionMode.AutoConnect
                || !state.AutomaticFanoutSubscriberRuntimes.TryGetValue(
                    channelName,
                    out var subscriberRuntime))
                continue;

            var loop = new SubscriberLoop(
                channelName,
                _store,
                subscriberRuntime,
                _options,
                _leases,
                state.ErrorSink);
            await loop.StartAsync(cancellationToken).ConfigureAwait(false);
            _subscribers.Add(loop);
        }
    }

    internal async ValueTask<bool> MarkDrainingAsync(
        CancellationToken cancellationToken)
    {
        var published = true;
        foreach (var publisher in _publishers)
        {
            publisher.Identity.MarkDraining();
            var result = await PublishAsync(
                    publisher,
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
        Action<ZLinkFanoutPublisherIdentity> transition,
        CancellationToken cancellationToken)
    {
        var published = true;
        foreach (var publisher in _publishers)
        {
            transition(publisher.Identity);
            var result = await PublishAsync(
                    publisher,
                    ZLinkLocationWriteIntent.Renew,
                    cancellationToken)
                .ConfigureAwait(false);
            published &= result.Status == ZLinkLocationWriteStatus.Stored;
        }
        return published;
    }

    public async ValueTask DisposeAsync()
    {
        var failures = new List<Exception>();
        foreach (var subscriber in _subscribers)
            try
            {
                await subscriber.DisposeAsync().ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                failures.Add(exception);
            }
        _subscribers.Clear();

        foreach (var publisher in _publishers)
            try
            {
                _ = await _store.RemoveFanoutPublisherAsync(
                        new ZLinkFanoutPublisherDescriptorKey(
                            publisher.Identity.ChannelName,
                            publisher.Identity.PublisherRid),
                        _locationRuntime.OwnerToken,
                        CancellationToken.None)
                    .ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                failures.Add(exception);
            }
        _publishers.Clear();

        if (failures.Count == 1)
            System.Runtime.ExceptionServices.ExceptionDispatchInfo
                .Capture(failures[0]).Throw();
        if (failures.Count > 1)
            throw new AggregateException(failures);
    }

    private async ValueTask<ZLinkLocationWriteResult> PublishAsync(
        Publisher publisher,
        ZLinkLocationWriteIntent intent,
        CancellationToken cancellationToken)
    {
        var owner = _locationRuntime.AdmissionOwnerToken;
        var identity = publisher.Identity;
        var snapshot = identity.Read();
        var descriptor = new ZLinkFanoutPublisherDescriptor(
            identity.ChannelName,
            identity.PublisherRid,
            identity.LifecycleGeneration,
            snapshot.DescriptorRevision,
            identity.Endpoint,
            snapshot.State,
            ZLinkTransportSecurityIdentity.Plaintext,
            owner.OwnerId,
            owner.LeaseGeneration,
            default);
        var result = await _store.UpdateFanoutPublisherAsync(
                descriptor,
                intent,
                cancellationToken)
            .ConfigureAwait(false);
        if (result.Status == ZLinkLocationWriteStatus.RejectedConflict
            && intent == ZLinkLocationWriteIntent.NewClaim)
            result = await _store.UpdateFanoutPublisherAsync(
                    descriptor,
                    ZLinkLocationWriteIntent.Takeover,
                    cancellationToken)
                .ConfigureAwait(false);
        if (result.Status != ZLinkLocationWriteStatus.Stored)
            throw new ZLinkConfigurationException(
                $"Fanout publisher descriptor '{identity.ChannelName}' could not be published.");
        return result;
    }

    private sealed record Publisher(ZLinkFanoutPublisherIdentity Identity);

    private sealed class SubscriberLoop(
        string channelName,
        IZLinkLocationRepository store,
        ZLinkAutomaticFanoutSubscriberRuntime runtime,
        ZLinkLocationOptions options,
        ZLinkOwnerLeaseTracker? leases,
        IZLinkRuntimeFailureReporter errorSink) : IAsyncDisposable
    {
        private readonly Dictionary<string, ulong> _observedRevisions =
            new(StringComparer.Ordinal);
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
            while (await timer.WaitForNextTickAsync(cancellationToken)
                       .ConfigureAwait(false))
                try
                {
                    await ReconcileAsync(cancellationToken).ConfigureAwait(false);
                }
                catch (OperationCanceledException)
                    when (cancellationToken.IsCancellationRequested)
                {
                    break;
                }
                catch (Exception exception)
                {
                    // A failed read keeps the last successful desired set.
                    var observed = DateTimeOffset.UtcNow;
                    runtime.RecordLocationFailure(
                        lastSuccessAt: null,
                        failureAt: observed);
                    errorSink.ReportRuntimeTaskException(
                        $"fanout-discovery:{channelName}",
                        exception);
                }
        }

        private async ValueTask ReconcileAsync(
            CancellationToken cancellationToken)
        {
            var rows = await ListAllAsync(cancellationToken).ConfigureAwait(false);
            var plans = new List<ZLinkFanoutConnectionPlan>(rows.Count);
            foreach (var row in rows)
            {
                if (row.PublisherRid.Size == 0
                    || row.LifecycleGeneration == 0
                    || row.DescriptorRevision == 0
                    || string.IsNullOrWhiteSpace(row.Endpoint))
                    continue;
                var ownerLive = leases is null
                    || await leases.IsOwnerTokenLiveAsync(
                            new ZLinkLocationOwnerToken(
                                row.OwnerId,
                                row.LeaseGeneration),
                            cancellationToken)
                        .ConfigureAwait(false);

                var key =
                    $"{row.PublisherRid.ToHex()}:{row.LifecycleGeneration}";
                if (_observedRevisions.TryGetValue(key, out var observed)
                    && row.DescriptorRevision < observed)
                {
                    plans.Add(new ZLinkFanoutConnectionPlan(
                        row,
                        Connect: false,
                        ZLinkFanoutPublisherConnectionState.ExcludedStale));
                    continue;
                }
                _observedRevisions[key] = row.DescriptorRevision;
                if (!ownerLive)
                {
                    plans.Add(new ZLinkFanoutConnectionPlan(
                        row,
                        Connect: false,
                        ZLinkFanoutPublisherConnectionState.ExcludedStale));
                }
                else if (row.State == ZLinkFrameworkRuntimeState.Draining)
                {
                    plans.Add(new ZLinkFanoutConnectionPlan(
                        row,
                        Connect: false,
                        ZLinkFanoutPublisherConnectionState.ExcludedDraining));
                }
                else if (row.State == ZLinkFrameworkRuntimeState.Serving)
                {
                    plans.Add(new ZLinkFanoutConnectionPlan(
                        row,
                        Connect: true,
                        ZLinkFanoutPublisherConnectionState.Connecting));
                }
            }

            await runtime.ReplaceAsync(
                plans,
                new ZLinkLocationRuntimeSnapshot(
                    "healthy",
                    DateTimeOffset.UtcNow,
                    null)).ConfigureAwait(false);
        }

        private async ValueTask<IReadOnlyList<ZLinkFanoutPublisherDescriptor>>
            ListAllAsync(CancellationToken cancellationToken)
        {
            var result = new List<ZLinkFanoutPublisherDescriptor>();
            string? continuation = null;
            do
            {
                var page = await store.ListFanoutPublishersAsync(
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

            await runtime.ReplaceAsync(
                    [],
                    new ZLinkLocationRuntimeSnapshot(
                        "healthy",
                        DateTimeOffset.UtcNow,
                        null))
                .ConfigureAwait(false);
        }
    }
}
