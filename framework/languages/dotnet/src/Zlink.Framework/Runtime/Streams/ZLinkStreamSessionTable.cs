using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.Runtime.Streams;

internal sealed class ZLinkStreamSessionTable(
    IServiceProvider services,
    IZLinkBackendStreamSocket socket,
    Type? headerSessionType,
    ZLinkDrainAdmissionGate drainAdmission,
    string transport,
    TimeProvider timeProvider,
    bool actorDispatchEnabled,
    ZLinkAsyncSubmitter sendSubmitter,
    ZLinkCompletionAdmissionOwner? completionAdmission)
{
    private readonly object _gate = new();
    private readonly Dictionary<string, ZLinkStreamSessionRuntime> _sessions = [];
    private readonly Dictionary<string, TaskCompletionSource<ZLinkStreamSessionRuntime?>>
        _sessionCreations = [];
    private bool _rejectNewSessions;
    private bool _stopping;

    public bool IsStopping
    {
        get
        {
            lock (_gate)
            {
                return _stopping;
            }
        }
    }

    public int Count
    {
        get
        {
            lock (_gate) return _sessions.Count;
        }
    }

    public ZLinkStreamSessionRuntime[] Snapshot()
    {
        lock (_gate) return _sessions.Values.ToArray();
    }

    public ZLinkStreamSessionRuntime[] Stop()
    {
        lock (_gate)
        {
            _stopping = true;
            var sessions = _sessions.Values.ToArray();
            _sessions.Clear();
            return sessions;
        }
    }

    public void RequestStop()
    {
        ZLinkStreamSessionRuntime[] sessions;
        lock (_gate) sessions = _sessions.Values.ToArray();
        foreach (var session in sessions) session.RequestStop();
    }

    public async ValueTask<ZLinkStreamSessionRuntime?> GetOrCreateAsync(RoutingId routingId)
    {
        var sessionId = routingId.ToHex();
        var reject = false;
        var creator = false;
        TaskCompletionSource<ZLinkStreamSessionRuntime?>? creation = null;
        lock (_gate)
        {
            if (_stopping) return null;

            if (_sessions.TryGetValue(sessionId, out var existing))
                return existing;
            reject = _rejectNewSessions || drainAdmission.IsDraining;
            if (!reject)
            {
                if (!_sessionCreations.TryGetValue(sessionId, out creation))
                {
                    creation = new TaskCompletionSource<ZLinkStreamSessionRuntime?>(
                        TaskCreationOptions.RunContinuationsAsynchronously);
                    _sessionCreations.Add(sessionId, creation);
                    creator = true;
                }
            }
        }
        if (reject)
        {
            RejectNewSession(routingId);
            return null;
        }

        if (creator)
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
                        sendSubmitter,
                        requireConnectionReady: true,
                        completionAdmission: completionAdmission)
                    .ConfigureAwait(false);
                var result = created;
                var disposeCreated = false;
                var rejectCreated = false;
                lock (_gate)
                {
                    _sessionCreations.Remove(sessionId);
                    if (_stopping)
                    {
                        result = null;
                        disposeCreated = true;
                    }
                    else if (_sessions.TryGetValue(sessionId, out var existing))
                    {
                        result = existing;
                        disposeCreated = true;
                    }
                    else if (_rejectNewSessions || drainAdmission.IsDraining)
                    {
                        result = null;
                        disposeCreated = true;
                        rejectCreated = true;
                    }
                    else
                        _sessions.Add(sessionId, created);
                }

                if (disposeCreated)
                    await created.DisposeUncommittedAsync().ConfigureAwait(false);
                if (rejectCreated) RejectNewSession(routingId);
                creation!.TrySetResult(result);
            }
            catch (Exception exception)
            {
                lock (_gate) _sessionCreations.Remove(sessionId);
                creation!.TrySetException(exception);
            }
        }

        return await creation!.Task.ConfigureAwait(false);
    }

    public bool TryGet(RoutingId routingId, out ZLinkStreamSessionRuntime session)
    {
        lock (_gate)
        {
            if (_sessions.TryGetValue(routingId.ToHex(), out var existing))
            {
                session = existing;
                return true;
            }
        }

        session = null!;
        return false;
    }

    public async ValueTask<bool> DrainSessionsAsync(CancellationToken cancellationToken)
    {
        ZLinkStreamSessionRuntime[] sessions;
        lock (_gate)
        {
            _rejectNewSessions = true;
            sessions = _sessions.Values.ToArray();
        }

        var allClosed = true;
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
            foreach (var closed in results) allClosed &= closed;
        }
        return allClosed;
    }

    public void ForceStopSessions()
    {
        ZLinkStreamSessionRuntime[] sessions;
        lock (_gate)
        {
            _rejectNewSessions = true;
            sessions = _sessions.Values.ToArray();
        }

        foreach (var session in sessions) session.RequestForceStopForDrain();
    }

    public bool TryResolveMonitorSession(
        RoutingId? routingId,
        out ZLinkStreamSessionRuntime session)
    {
        lock (_gate)
        {
            if (routingId is RoutingId streamRoutingId
                && _sessions.TryGetValue(streamRoutingId.ToHex(), out var existing))
            {
                session = existing;
                return true;
            }
        }

        session = null!;
        return false;
    }

    private void Remove(string sessionId)
    {
        lock (_gate)
        {
            _sessions.Remove(sessionId);
        }
    }

    private void RejectNewSession(RoutingId routingId)
    {
        try
        {
            var payload = ZLinkStreamSessionClosingCodec.EncodeServerDrain();
            ZLinkStreamFrameWriter.Write(
                message => socket.Send(routingId, message, SendFlags.None),
                ZLinkStreamSessionClosingCodec.CreateHeader(),
                payload,
                "Could not submit the session-closing control packet.");
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
