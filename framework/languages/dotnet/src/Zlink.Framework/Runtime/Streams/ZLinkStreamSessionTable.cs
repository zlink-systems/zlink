using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Streams;

internal sealed class ZLinkStreamSessionTable(
    IServiceProvider services,
    IZLinkBackendStreamSocket socket,
    Type? headerSessionType,
    ZLinkDrainAdmissionGate drainAdmission,
    string transport,
    TimeProvider timeProvider,
    bool actorDispatchEnabled)
{
    private readonly ZLinkStateLane _lane = new();
    private readonly Dictionary<RoutingId, ZLinkStreamSessionRuntime> _sessions = [];
    private readonly Dictionary<RoutingId, TaskCompletionSource<ZLinkStreamSessionRuntime?>>
        _sessionCreations = [];
    private CancellationToken? _rejectionCancellationToken;
    private bool _stopping;

    public ValueTask<bool> IsStoppingAsync() => _lane.RunAsync(() => _stopping);

    public ValueTask<int> GetCountAsync() => _lane.RunAsync(() => _sessions.Count);

    public ValueTask<ZLinkStreamSessionRuntime[]> SnapshotAsync() =>
        _lane.RunAsync(() => _sessions.Values.ToArray());

    public ValueTask<ZLinkStreamSessionRuntime[]> StopAsync() =>
        _lane.RunAsync(StopCore);

    public ValueTask SealAdmissionAsync(CancellationToken cancellationToken) =>
        _lane.RunAsync(() =>
        {
            _rejectionCancellationToken = cancellationToken;
        });

    public async ValueTask RequestStopAsync()
    {
        var sessions = await _lane.RunAsync(() => _sessions.Values.ToArray()).ConfigureAwait(false);
        foreach (var session in sessions) session.RequestStop();
    }

    public async ValueTask<ZLinkStreamSessionRuntime?> GetOrCreateAsync(
        RoutingId routingId,
        CancellationToken cancellationToken)
    {
        var admission = await _lane.RunAsync(
                () => PrepareGetOrCreate(routingId, cancellationToken))
            .ConfigureAwait(false);
        if (admission.Existing is { } existing)
            return existing;
        if (admission.Stopped)
            return null;
        if (admission.Reject)
        {
            await RejectNewSessionAsync(routingId, admission.RejectionCancellationToken)
                .ConfigureAwait(false);
            return null;
        }

        if (admission.Creator)
        {
            try
            {
                var created = await ZLinkStreamSessionRuntime.CreateAsync(
                        services,
                        socket,
                        routingId,
                        headerSessionType,
                        Remove,
                        transport,
                        timeProvider,
                        actorDispatchEnabled,
                        requireConnectionReady: true)
                    .ConfigureAwait(false);
                var completion = await _lane.RunAsync(
                        () => CompleteCreation(routingId, created, cancellationToken))
                    .ConfigureAwait(false);

                if (completion.DisposeCreated)
                    await created.DisposeUncommittedAsync().ConfigureAwait(false);
                if (completion.RejectCreated)
                    await RejectNewSessionAsync(
                            routingId,
                            completion.RejectionCancellationToken)
                        .ConfigureAwait(false);
                admission.Creation!.TrySetResult(completion.Result);
            }
            catch (Exception exception)
            {
                await _lane.RunAsync(() => _sessionCreations.Remove(routingId))
                    .ConfigureAwait(false);
                admission.Creation!.TrySetException(exception);
            }
        }

        return await admission.Creation!.Task.ConfigureAwait(false);
    }

    public ValueTask<ZLinkStreamSessionRuntime?> TryGetAsync(RoutingId routingId) =>
        _lane.RunAsync(() => _sessions.GetValueOrDefault(routingId));

    public async ValueTask<bool> DrainSessionsAsync(CancellationToken cancellationToken)
    {
        var sessions = await _lane.RunAsync(() =>
        {
            _rejectionCancellationToken = cancellationToken;
            return _sessions.Values.ToArray();
        }).ConfigureAwait(false);

        var allClosed = true;
        var failures = new List<Exception>();
        const int maximumConcurrency = 16;
        for (var offset = 0; offset < sessions.Length; offset += maximumConcurrency)
        {
            var count = Math.Min(maximumConcurrency, sessions.Length - offset);
            var closes = new Task<bool>[count];
            for (var index = 0; index < count; index++)
                closes[index] = sessions[offset + index]
                    .CloseForDrainAsync(cancellationToken)
                    .AsTask();
            var results = await Task.WhenAll(closes).ConfigureAwait(false);
            for (var index = 0; index < results.Length; index++)
            {
                if (results[index]) continue;
                allClosed = false;
                if (sessions[offset + index].TerminalFailure is { } failure)
                    failures.Add(failure);
            }
        }
        if (failures.Count > 0 && !cancellationToken.IsCancellationRequested)
            throw new ZLinkDrainForceException(
                ZLinkDrainForceReason.TeardownFailed,
                failures);
        return allClosed;
    }

    public async ValueTask ForceStopSessionsAsync(CancellationToken cancellationToken)
    {
        var sessions = await _lane.RunAsync(() =>
        {
            _rejectionCancellationToken = cancellationToken;
            return _sessions.Values.ToArray();
        }).ConfigureAwait(false);

        await Task.WhenAll(sessions.Select(session =>
                session.RequestForceStopForDrainAsync(cancellationToken).AsTask()))
            .ConfigureAwait(false);
    }

    public ValueTask<ZLinkStreamSessionRuntime?> TryResolveMonitorSessionAsync(
        RoutingId? routingId) => _lane.RunAsync(() =>
            routingId is RoutingId streamRoutingId
                ? _sessions.GetValueOrDefault(streamRoutingId)
                : null);

    private void Remove(string sessionId)
    {
        var routingId = RoutingId.FromHex(sessionId);
        AwaitStateLane(_lane.RunAsync(() => { _sessions.Remove(routingId); }));
    }

    private ZLinkStreamSessionRuntime[] StopCore()
    {
        _stopping = true;
        var sessions = _sessions.Values.ToArray();
        _sessions.Clear();
        return sessions;
    }

    private SessionCreationAdmission PrepareGetOrCreate(
        RoutingId routingId,
        CancellationToken cancellationToken)
    {
        if (_stopping)
            return new(null, null, false, false, true, cancellationToken);

        if (_sessions.TryGetValue(routingId, out var existing))
            return new(existing, null, false, false, false, cancellationToken);
        var reject = _rejectionCancellationToken.HasValue || drainAdmission.IsDraining;
        if (reject)
            return new(
                null,
                null,
                true,
                false,
                false,
                _rejectionCancellationToken ?? cancellationToken);

        if (!_sessionCreations.TryGetValue(routingId, out var creation))
        {
            creation = new TaskCompletionSource<ZLinkStreamSessionRuntime?>(
                TaskCreationOptions.RunContinuationsAsynchronously);
            _sessionCreations.Add(routingId, creation);
            return new(null, creation, false, true, false, cancellationToken);
        }

        return new(null, creation, false, false, false, cancellationToken);
    }

    private SessionCreationCompletion CompleteCreation(
        RoutingId routingId,
        ZLinkStreamSessionRuntime created,
        CancellationToken cancellationToken)
    {
        _sessionCreations.Remove(routingId);
        if (_stopping)
            return new(null, true, false, CancellationToken.None);
        if (_sessions.TryGetValue(routingId, out var existing))
            return new(existing, true, false, CancellationToken.None);
        if (_rejectionCancellationToken.HasValue || drainAdmission.IsDraining)
            return new(
                null,
                true,
                true,
                _rejectionCancellationToken ?? cancellationToken);

        _sessions.Add(routingId, created);
        return new(created, false, false, CancellationToken.None);
    }

    private static void AwaitStateLane(ValueTask operation) =>
        operation.GetAwaiter().GetResult();

    private readonly record struct SessionCreationAdmission(
        ZLinkStreamSessionRuntime? Existing,
        TaskCompletionSource<ZLinkStreamSessionRuntime?>? Creation,
        bool Reject,
        bool Creator,
        bool Stopped,
        CancellationToken RejectionCancellationToken);

    private readonly record struct SessionCreationCompletion(
        ZLinkStreamSessionRuntime? Result,
        bool DisposeCreated,
        bool RejectCreated,
        CancellationToken RejectionCancellationToken);

    private async ValueTask RejectNewSessionAsync(
        RoutingId routingId,
        CancellationToken cancellationToken)
    {
        try
        {
            var payload = ZlinkStreamSessionClosingCodec.EncodeServerDrain();
            await ZLinkStreamFrameWriter.WriteAsync(
                    (message, token) => socket.SendAsync(routingId, message, token),
                    ZlinkStreamSessionClosingCodec.CreateHeader(),
                    payload,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch
        {
        }
        finally
        {
            socket.DisconnectPeer(routingId);
        }
    }
}
