using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Messaging;
using static Zlink.Framework.Runtime.Execution.ZLinkStateLaneWait;

namespace Zlink.Framework.Runtime.Streams;

internal sealed class ZLinkStreamSessionTable(
    IServiceProvider services,
    IZLinkBackendStreamSocket socket,
    Type? headerSessionType,
    ZLinkDrainAdmissionGate drainAdmission,
    string transport,
    TimeProvider timeProvider,
    bool actorDispatchEnabled,
    IZLinkRuntimeFailureReporter errorSink,
    ZLinkStreamNodeRegistration? livenessOptions = null,
    ZLinkStreamSessionLiveness.Reader? readerLiveness = null
)
{
    private const int RejectedCreationLimit = 4096;
    private readonly ZLinkStateLane _lane = new();
    private readonly Dictionary<RoutingId, ZLinkStreamSessionRuntime> _sessions = [];
    private readonly Queue<ZLinkStreamSessionRuntime.LivenessTimer> _dueLiveness = [];
    private readonly Dictionary<
        RoutingId,
        TaskCompletionSource<ZLinkStreamSessionRuntime?>
    > _sessionCreations = [];
    private readonly HashSet<RoutingId> _rejectedCreations = [];
    private readonly Queue<RoutingId> _rejectedCreationOrder = [];
    private CancellationToken? _rejectionCancellationToken;
    private bool _stopping;

    public ValueTask<bool> IsStoppingAsync() => _lane.RunAsync(() => _stopping);

    public ValueTask<int> GetCountAsync() => _lane.RunAsync(() => _sessions.Count);

    public ValueTask<ZLinkStreamSessionRuntime[]> StopAsync() => _lane.RunAsync(StopCore);

    public ValueTask SealAdmissionAsync(CancellationToken cancellationToken) =>
        _lane.RunAsync(() =>
        {
            _rejectionCancellationToken = cancellationToken;
        });

    public async ValueTask RequestStopAsync()
    {
        var sessions = await _lane.RunAsync(() => _sessions.Values.ToArray()).ConfigureAwait(false);
        foreach (var session in sessions)
            session.RequestStop();
    }

    public async ValueTask<ZLinkStreamSessionRuntime?> GetOrCreateAsync(
        RoutingId routingId,
        CancellationToken cancellationToken,
        ZlinkStreamHeader? firstPacket = null
    )
    {
        var admission = await _lane
            .RunAsync(() => PrepareGetOrCreate(routingId, cancellationToken))
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
                var created = await TryCreateSessionAsync(routingId, firstPacket, cancellationToken)
                    .ConfigureAwait(false);
                if (created is null)
                {
                    admission.Creation!.TrySetResult(null);
                    return null;
                }
                var completion = await _lane
                    .RunAsync(() => CompleteCreation(routingId, created, cancellationToken))
                    .ConfigureAwait(false);

                if (completion.DisposeCreated)
                    await created.DisposeUncommittedAsync().ConfigureAwait(false);
                if (completion.RejectCreated)
                    await RejectNewSessionAsync(routingId, completion.RejectionCancellationToken)
                        .ConfigureAwait(false);
                admission.Creation!.TrySetResult(completion.Result);
            }
            catch (Exception exception)
            {
                await _lane
                    .RunAsync(() => _sessionCreations.Remove(routingId))
                    .ConfigureAwait(false);
                admission.Creation!.TrySetException(exception);
            }
        }

        return await admission.Creation!.Task.ConfigureAwait(false);
    }

    private async ValueTask<ZLinkStreamSessionRuntime?> TryCreateSessionAsync(
        RoutingId routingId,
        ZlinkStreamHeader? firstPacket,
        CancellationToken cancellationToken
    )
    {
        try
        {
            return await ZLinkStreamSessionRuntime
                .CreateAsync(
                    services,
                    socket,
                    routingId,
                    headerSessionType,
                    Remove,
                    transport,
                    timeProvider,
                    actorDispatchEnabled,
                    requireConnectionReady: true,
                    livenessOptions: livenessOptions,
                    readerLiveness: readerLiveness,
                    markLivenessDue: readerLiveness is null ? null : MarkLivenessDue
                )
                .ConfigureAwait(false);
        }
        catch (Exception failure)
        {
            await _lane.RunAsync(() => _rejectedCreations.Add(routingId)).ConfigureAwait(false);
            try
            {
                await RejectFailedCreationAsync(routingId, firstPacket, failure, cancellationToken)
                    .ConfigureAwait(false);
            }
            finally
            {
                await _lane
                    .RunAsync(() =>
                    {
                        // Keep rejected admissions through close and subsequent queued ingress.
                        if (_rejectedCreationOrder.Count == RejectedCreationLimit)
                            _rejectedCreations.Remove(_rejectedCreationOrder.Dequeue());
                        _rejectedCreationOrder.Enqueue(routingId);
                        _sessionCreations.Remove(routingId);
                    })
                    .ConfigureAwait(false);
            }
            return null;
        }
    }

    private async ValueTask RejectFailedCreationAsync(
        RoutingId routingId,
        ZlinkStreamHeader? firstPacket,
        Exception failure,
        CancellationToken cancellationToken
    )
    {
        const string diagnostic = "STREAM session creation failed.";
        var logger = ZLinkMessageFlowTracer.GetLogger(services);
        try
        {
            logger.LogError(
                failure,
                "STREAM session creation failed for node {NodeName}, session type {SessionType}, peer {PeerRoutingId}.",
                livenessOptions?.StreamNodeName,
                headerSessionType?.FullName,
                routingId
            );
            if (firstPacket is { } packet)
            {
                var flow = services.GetRequiredService<ZLinkFrameworkRuntime>().Flow;
                using var scope = ZLinkFlowContext.Enter(
                    packet.FlowId,
                    packet.FlowOrigin is { } origin
                        ? ZLinkStreamHeaderCodec.ToFrameworkOrigin(origin)
                        : null,
                    flow.CaptureEnabled,
                    ZLinkFlowOrigin.Inbound
                );
                if (flow.DispatchErrorsEnabled)
                    flow.TraceDispatchError(
                        new ZLinkDispatchFailure(
                            ZLinkDispatchErrorSurface.StreamSession,
                            packet.RequestSeq.HasValue
                                ? ZLinkDispatchMessageKind.Request
                                : ZLinkDispatchMessageKind.Send,
                            ZLinkDispatchErrorReason.SessionCreationFailed,
                            packet.RequestSeq.HasValue
                                ? ZLinkDispatchErrorAction.ReplyError
                                : ZLinkDispatchErrorAction.Drop,
                            packet.Name,
                            CorrelationId: packet.CorrelationId,
                            Exception: failure,
                            StreamSessionId: routingId.ToHex()
                        )
                    );
                if (packet.RequestSeq.HasValue)
                {
                    var reply = ZLinkActorReply.FromError(
                        new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.InternalFailure,
                            diagnostic
                        )
                    );
                    await socket
                        .SendAsync(
                            routingId,
                            Message.From(reply.ToFrame(packet)),
                            cancellationToken
                        )
                        .ConfigureAwait(false);
                }
            }
        }
        catch (Exception replyFailure)
        {
            errorSink.ReportRuntimeTaskException(nameof(RejectFailedCreationAsync), replyFailure);
        }

        await CloseRejectedSessionAsync(
                routingId,
                ZlinkStreamSessionClosingCodec.EncodeTransportError(diagnostic),
                cancellationToken
            )
            .ConfigureAwait(false);
    }

    public ValueTask<ZLinkStreamSessionRuntime?> TryGetAsync(RoutingId routingId) =>
        _lane.RunAsync(() => _sessions.GetValueOrDefault(routingId));

    private void MarkLivenessDue(ZLinkStreamSessionRuntime.LivenessTimer timer) =>
        _lane.TryPost(
            due =>
            {
                if (_stopping)
                    return;
                _dueLiveness.Enqueue(due);
                readerLiveness!.Wake();
            },
            timer
        );

    // Notifications added while the reader consumes this prefix belong to the next wake.
    public ValueTask<int> TakeDueLivenessWakeAsync() =>
        _lane.RunAsync(() => readerLiveness!.TakeWake() ? _dueLiveness.Count : 0);

    public ValueTask<ZLinkStreamSessionRuntime.LivenessTimer?> DequeueDueLivenessAsync() =>
        _lane.RunAsync(static queue => queue.TryDequeue(out var due) ? due : null, _dueLiveness);

    public ValueTask<ZLinkStreamSessionRuntime[]> RestartLivenessAsync() =>
        _lane.RunAsync(() =>
        {
            // The permit grant resets receive deadlines for every session on this reader.
            readerLiveness!.TakeWake();
            _dueLiveness.Clear();
            return _sessions.Values.ToArray();
        });

    public async ValueTask<bool> DrainSessionsAsync(CancellationToken cancellationToken)
    {
        var sessions = await _lane
            .RunAsync(() =>
            {
                _rejectionCancellationToken = cancellationToken;
                return _sessions.Values.ToArray();
            })
            .ConfigureAwait(false);

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
                if (results[index])
                    continue;
                allClosed = false;
                if (sessions[offset + index].TerminalFailure is { } failure)
                    failures.Add(failure);
            }
        }
        if (failures.Count > 0 && !cancellationToken.IsCancellationRequested)
            throw new ZLinkDrainForceException(ZLinkDrainForceReason.TeardownFailed, failures);
        return allClosed;
    }

    public async ValueTask ForceStopSessionsAsync(CancellationToken cancellationToken)
    {
        var sessions = await _lane
            .RunAsync(() =>
            {
                _rejectionCancellationToken = cancellationToken;
                return _sessions.Values.ToArray();
            })
            .ConfigureAwait(false);

        await Task.WhenAll(
                sessions.Select(session =>
                    session.RequestForceStopForDrainAsync(cancellationToken).AsTask()
                )
            )
            .ConfigureAwait(false);
    }

    public ValueTask<ZLinkStreamSessionRuntime?> TryResolveMonitorSessionAsync(
        RoutingId? routingId
    ) =>
        _lane.RunAsync(() =>
            routingId is RoutingId streamRoutingId
                ? _sessions.GetValueOrDefault(streamRoutingId)
                : null
        );

    private void Remove(string sessionId)
    {
        var routingId = RoutingId.FromHex(sessionId);
        AwaitStateLane(
            _lane.RunAsync(() =>
            {
                _sessions.Remove(routingId);
            })
        );
    }

    private ZLinkStreamSessionRuntime[] StopCore()
    {
        _stopping = true;
        _dueLiveness.Clear();
        var sessions = _sessions.Values.ToArray();
        _sessions.Clear();
        return sessions;
    }

    private SessionCreationAdmission PrepareGetOrCreate(
        RoutingId routingId,
        CancellationToken cancellationToken
    )
    {
        if (_stopping || _rejectedCreations.Contains(routingId))
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
                _rejectionCancellationToken ?? cancellationToken
            );

        if (!_sessionCreations.TryGetValue(routingId, out var creation))
        {
            creation = new TaskCompletionSource<ZLinkStreamSessionRuntime?>(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            _sessionCreations.Add(routingId, creation);
            return new(null, creation, false, true, false, cancellationToken);
        }

        return new(null, creation, false, false, false, cancellationToken);
    }

    private SessionCreationCompletion CompleteCreation(
        RoutingId routingId,
        ZLinkStreamSessionRuntime created,
        CancellationToken cancellationToken
    )
    {
        _sessionCreations.Remove(routingId);
        if (_stopping)
            return new(null, true, false, CancellationToken.None);
        if (_sessions.TryGetValue(routingId, out var existing))
            return new(existing, true, false, CancellationToken.None);
        if (_rejectionCancellationToken.HasValue || drainAdmission.IsDraining)
            return new(null, true, true, _rejectionCancellationToken ?? cancellationToken);

        _sessions.Add(routingId, created);
        return new(created, false, false, CancellationToken.None);
    }

    private readonly record struct SessionCreationAdmission(
        ZLinkStreamSessionRuntime? Existing,
        TaskCompletionSource<ZLinkStreamSessionRuntime?>? Creation,
        bool Reject,
        bool Creator,
        bool Stopped,
        CancellationToken RejectionCancellationToken
    );

    private readonly record struct SessionCreationCompletion(
        ZLinkStreamSessionRuntime? Result,
        bool DisposeCreated,
        bool RejectCreated,
        CancellationToken RejectionCancellationToken
    );

    private ValueTask RejectNewSessionAsync(
        RoutingId routingId,
        CancellationToken cancellationToken
    ) =>
        CloseRejectedSessionAsync(
            routingId,
            ZlinkStreamSessionClosingCodec.EncodeServerDrain(),
            cancellationToken
        );

    private async ValueTask CloseRejectedSessionAsync(
        RoutingId routingId,
        byte[] payload,
        CancellationToken cancellationToken
    )
    {
        try
        {
            await ZLinkStreamFrameWriter
                .WriteAsync(
                    (message, token) => socket.SendAsync(routingId, message, token),
                    ZlinkStreamSessionClosingCodec.CreateHeader(),
                    payload,
                    cancellationToken
                )
                .ConfigureAwait(false);
        }
        catch (Exception failure)
        {
            errorSink.ReportRuntimeTaskException(nameof(CloseRejectedSessionAsync), failure);
        }
        finally
        {
            socket.DisconnectPeer(routingId);
        }
    }
}
