using System.Collections.Concurrent;

namespace Zlink.Framework.Runtime.Service;

internal sealed class RawMeshMonitor : IMeshNodeMonitor
{
    private readonly ConcurrentQueue<MeshMonitorEvent> _events = new();
    private readonly MeshMonitorEventMask _mask;
    private long _sequence;
    private int _disposed;
    private MeshNodeState _state = MeshNodeState.Created;
    private long _admitted;
    private long _rejected;
    private long _submitted;
    private long _completed;
    private long _protocolErrors;
    private long _backpressured;

    internal RawMeshMonitor(MeshMonitorEventMask mask = MeshMonitorEventMask.All)
    {
        _mask = mask;
    }

    public MeshMonitorStatus Status() =>
        new(
            _state,
            checked((ulong)Math.Max(0, Interlocked.Read(ref _admitted))),
            checked((ulong)Math.Max(0, Interlocked.Read(ref _rejected))),
            checked((ulong)Math.Max(0, Interlocked.Read(ref _submitted))),
            checked((ulong)Math.Max(0, Interlocked.Read(ref _completed))),
            checked((ulong)Math.Max(0, Interlocked.Read(ref _protocolErrors))),
            checked((ulong)Math.Max(0, Interlocked.Read(ref _backpressured))),
            checked((ulong)Math.Max(0, Interlocked.Read(ref _sequence))));

    public MeshMonitorEvent? Recv(RecvFlags flags = RecvFlags.None)
    {
        if (Volatile.Read(ref _disposed) != 0)
            return null;
        return _events.TryDequeue(out var value) ? value : null;
    }

    internal void Publish(
        MeshMonitorEventKind kind,
        MeshNodeState state,
        RoutingId peerRid = default,
        string channelName = "",
        MeshOperationId operationId = default,
        int resultCode = 0,
        int failureErrno = 0)
    {
        _state = state;
        switch (kind)
        {
            case MeshMonitorEventKind.PeerAdmitted:
                Interlocked.Increment(ref _admitted);
                break;
            case MeshMonitorEventKind.PeerRejected:
                Interlocked.Increment(ref _rejected);
                break;
            case MeshMonitorEventKind.MessageSubmitted:
                Interlocked.Increment(ref _submitted);
                break;
            case MeshMonitorEventKind.OperationCompleted:
                Interlocked.Increment(ref _completed);
                break;
            case MeshMonitorEventKind.ProtocolError:
                Interlocked.Increment(ref _protocolErrors);
                break;
            case MeshMonitorEventKind.Backpressured:
                Interlocked.Increment(ref _backpressured);
                break;
        }

        if (Volatile.Read(ref _disposed) != 0 || !Includes(kind))
            return;
        var sequence = checked((ulong)Interlocked.Increment(ref _sequence));
        _events.Enqueue(new MeshMonitorEvent(
            kind,
            checked((ulong)Environment.TickCount64),
            0,
            0,
            state,
            peerRid,
            0,
            0,
            MeshOwnerKind.Node,
            string.Empty,
            default,
            channelName,
            operationId,
            resultCode,
            failureErrno));
        _ = sequence;
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
            return;
        while (_events.TryDequeue(out _))
        {
        }
    }

    public ValueTask DisposeAsync()
    {
        Dispose();
        return ValueTask.CompletedTask;
    }

    private bool Includes(MeshMonitorEventKind kind)
    {
        var bit = kind switch
        {
            MeshMonitorEventKind.StateChanged => MeshMonitorEventMask.StateChanged,
            MeshMonitorEventKind.PeerConnecting => MeshMonitorEventMask.PeerConnecting,
            MeshMonitorEventKind.PeerAdmitted => MeshMonitorEventMask.PeerAdmitted,
            MeshMonitorEventKind.PeerDraining => MeshMonitorEventMask.PeerDraining,
            MeshMonitorEventKind.PeerClosed => MeshMonitorEventMask.PeerClosed,
            MeshMonitorEventKind.PeerRejected => MeshMonitorEventMask.PeerRejected,
            MeshMonitorEventKind.ChannelChanged => MeshMonitorEventMask.ChannelChanged,
            MeshMonitorEventKind.MessageSubmitted => MeshMonitorEventMask.MessageSubmitted,
            MeshMonitorEventKind.Backpressured => MeshMonitorEventMask.Backpressured,
            MeshMonitorEventKind.OperationCompleted => MeshMonitorEventMask.OperationCompleted,
            MeshMonitorEventKind.ProtocolError => MeshMonitorEventMask.ProtocolError,
            MeshMonitorEventKind.ClaimRevoked => MeshMonitorEventMask.ClaimRevoked,
            MeshMonitorEventKind.PeerNotRequired => MeshMonitorEventMask.PeerNotRequired,
            _ => MeshMonitorEventMask.None
        };
        return (_mask & bit) != 0;
    }
}
