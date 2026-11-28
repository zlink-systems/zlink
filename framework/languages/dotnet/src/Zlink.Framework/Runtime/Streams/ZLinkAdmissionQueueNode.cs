using System.Text.Json;
using Systems.Zlink.Stream.Connector.Runtime.Protocol;

namespace Zlink.Framework.Runtime.Streams;

internal sealed class ZLinkAdmissionQueueNode(
    ZLinkAdmissionQueueConfiguration configuration,
    ZLinkSpotNodeRuntime mesh,
    ZLinkLocationLifecycle location,
    IZLinkSpotClient client,
    ZLinkFrameworkComponentState state,
    ZLinkAuthenticationAdmission authentication,
    int localLimit
)
{
    private readonly ZLinkStateLane _lane = new();
    private readonly string _spotId = AdmissionQueueMessages.SpotId(configuration.MeshName);
    private readonly string _nodeRid = ZLinkAdmissionTicket.Encode(mesh.Node.RoutingId.ToBytes());
    private readonly SortedSet<ulong> _resolved = [];
    private readonly Dictionary<ulong, ulong> _held = [];
    private readonly HashSet<AdmissionSession> _used = [];
    private readonly HashSet<AdmissionSession> _ended = [];
    private ulong _epoch;
    private ulong _lastKnownEpoch;
    private ulong _nextRequestSeq;
    private ulong _resolvedPrefix;

    internal const string TicketKey = ZlinkStreamHeaderCodec.AdmissionTicketName;
    internal const string EnqueueName = ZlinkStreamHeaderCodec.AdmissionEnqueueName;
    internal static ZlinkStreamMetadata QueueRequired { get; } =
        ZlinkStreamMetadata.Empty.With("$zlink.admission.queueRequired", "1");

    private ulong Generation => mesh.Node.MeshStatus().LifecycleGeneration;

    internal AdmissionConnection Connection(string streamName, RoutingId sessionRid) =>
        new(_nodeRid, Generation, streamName, ZLinkAdmissionTicket.Encode(sessionRid.ToBytes()));

    internal ulong Verify(string? ticket, string streamName, RoutingId sessionRid)
    {
        var epoch = Volatile.Read(ref _epoch);
        if (epoch == 0 || ticket is null)
            return 0;
        return configuration.Ticket.Verify(
            ticket,
            Connection(streamName, sessionRid),
            epoch,
            state.Registration.TimeProvider.GetUtcNow().ToUnixTimeMilliseconds()
        )
            ? epoch
            : 0;
    }

    internal async ValueTask<AdmissionDelivery> EnqueueAsync(
        string streamName,
        RoutingId sessionRid,
        CancellationToken cancellationToken
    )
    {
        try
        {
            var response = await RequestAsync<AdmissionEnqueue, AdmissionEnqueued>(
                    new AdmissionEnqueue(Connection(streamName, sessionRid)),
                    cancellationToken
                )
                .ConfigureAwait(false);
            return response.Delivery;
        }
        catch (Exception error) when (IsSpotRequestFailure(error, cancellationToken))
        {
            Volatile.Write(ref _epoch, 0);
            throw;
        }
    }

    internal async ValueTask<PermitDecision> AdmitAsync(
        string streamName,
        RoutingId sessionRid,
        ulong epoch,
        CancellationToken cancellationToken
    )
    {
        if (configuration.GlobalLimit is null)
            return new PermitDecision(true, false, 0);

        var owner = location.OwnerToken;
        if (!location.IsOwnerLeaseValid(owner))
            return default;

        var connection = Connection(streamName, sessionRid);
        var sequence = await _lane.RunAsync(() => checked(++_nextRequestSeq)).ConfigureAwait(false);
        ulong permit = 0;
        try
        {
            var result = await RequestAsync<AdmissionPermitRequest, AdmissionPermitResult>(
                    new AdmissionPermitRequest(
                        _nodeRid,
                        connection.NodeLifecycleGeneration,
                        owner.OwnerId,
                        checked((ulong)owner.LeaseGeneration),
                        streamName,
                        connection.SessionRid,
                        epoch,
                        sequence
                    ),
                    cancellationToken
                )
                .ConfigureAwait(false);
            permit = result.Granted ? result.PermitId.GetValueOrDefault() : 0;
            return result.Granted
                ? new PermitDecision(true, false, permit)
                : new PermitDecision(false, true, 0);
        }
        catch (Exception error) when (IsSpotRequestFailure(error, cancellationToken))
        {
            state.ErrorSink.ReportRuntimeTaskException("admission-queue-admit-permit", error);
            return default;
        }
        finally
        {
            await _lane
                .RunAsync(() =>
                {
                    if (permit != 0 && _lastKnownEpoch == epoch)
                        _held.Add(permit, epoch);
                    _resolved.Add(sequence);
                    while (_resolved.Remove(_resolvedPrefix + 1))
                        _resolvedPrefix++;
                })
                .ConfigureAwait(false);
        }
    }

    internal ValueTask UsedAsync(string streamName, RoutingId sessionRid) =>
        _lane.RunAsync(() =>
        {
            _used.Add(Session(streamName, sessionRid));
        });

    internal ValueTask EndedAsync(string streamName, RoutingId sessionRid) =>
        _lane.RunAsync(() =>
        {
            _ended.Add(Session(streamName, sessionRid));
        });

    internal async ValueTask ReleaseAsync(ulong id)
    {
        if (id == 0 || !await _lane.RunAsync(() => _held.Remove(id)).ConfigureAwait(false))
            return;
        state.TaskRunner.RunDetached(
            "admission-queue-release-permit",
            async _ =>
            {
                await client
                    .SendToSpot(_spotId, new AdmissionPermitRelease(_nodeRid, Generation, id))
                    .InMesh(configuration.MeshName)
                    .InstanceSpot(AdmissionQueueMessages.SpotType)
                    .Async()
                    .ConfigureAwait(false);
            }
        );
    }

    internal async ValueTask ReportAsync(ZLinkTimerTick tick, CancellationToken cancellationToken)
    {
        var snapshot = await _lane
            .RunAsync(() =>
            {
                var report = new AdmissionReport(
                    _nodeRid,
                    Generation,
                    localLimit,
                    authentication.InFlight,
                    _used.ToArray(),
                    _ended.ToArray(),
                    _held.Keys.ToArray(),
                    _resolvedPrefix
                );
                _used.Clear();
                _ended.Clear();
                return report;
            })
            .ConfigureAwait(false);
        AdmissionReported result;
        try
        {
            result = await RequestAsync<AdmissionReport, AdmissionReported>(
                    snapshot,
                    cancellationToken
                )
                .ConfigureAwait(false);
        }
        catch (Exception error) when (IsSpotRequestFailure(error, cancellationToken))
        {
            await _lane
                .RunAsync(() =>
                {
                    Volatile.Write(ref _epoch, 0);
                    _used.UnionWith(snapshot.UsedTickets);
                    _ended.UnionWith(snapshot.EndedSessions);
                })
                .ConfigureAwait(false);
            state.ErrorSink.ReportRuntimeTaskException("admission-queue-report", error);
            return;
        }

        await _lane
            .RunAsync(() =>
            {
                foreach (var (id, epoch) in _held)
                    if (epoch != result.Epoch)
                        _held.Remove(id);
                _lastKnownEpoch = result.Epoch;
                Volatile.Write(ref _epoch, result.Epoch);
            })
            .ConfigureAwait(false);
        foreach (var delivery in result.Deliveries)
            if (state.StreamNodes.TryGetValue(delivery.StreamName, out var stream))
                await stream
                    .DeliverAdmissionAsync(delivery, cancellationToken)
                    .ConfigureAwait(false);
    }

    internal static bool IsSpotRequestFailure(
        Exception error,
        CancellationToken cancellationToken
    ) =>
        error
            is ZLinkFrameworkException
                or ZlinkSubmitException
                or TimeoutException
                or JsonException
        || (error is OperationCanceledException && !cancellationToken.IsCancellationRequested);

    private async ValueTask<TReply> RequestAsync<TRequest, TReply>(
        TRequest request,
        CancellationToken cancellationToken
    ) =>
        await client
            .RequestToSpot(_spotId, request)
            .InMesh(configuration.MeshName)
            .InstanceSpot(AdmissionQueueMessages.SpotType)
            .Async<TReply>(cancellationToken);

    private static AdmissionSession Session(string streamName, RoutingId sessionRid) =>
        new(streamName, ZLinkAdmissionTicket.Encode(sessionRid.ToBytes()));

    internal readonly record struct PermitDecision(bool Granted, bool QueueRequired, ulong Id);
}
