using System.Threading.Tasks.Sources;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Backend.DotNet.Wrappers;

// One Framework receive owner registers one native socket with one public ZLink
// poller. Completion-capable sockets use that same poller as their sole async
// completion owner.
internal sealed class ZLinkBackendSocketPoller
    : IZLinkBackendSocketPoller,
        IValueTaskSource<ZLinkBackendSocketReadiness>
{
    private const nuint Slot = 1;
    private readonly ZLinkProcessExecutionPool.ReceiveSource _source;
    private readonly ZLinkProcessExecutionPool.ReceiveEvent[] _events =
        new ZLinkProcessExecutionPool.ReceiveEvent[1];

    private ZLinkBackendSocketPoller(ZLinkProcessExecutionPool.ReceiveSource source) =>
        _source = source;

    internal static IZLinkBackendSocketPoller Create(IZlinkSocket socket)
    {
        var source = ZLinkProcessExecutionPool.CreateReceiveSource();
        var events = PollEventFlags.PollIn;
        if (socket is IPairSocket or IDealerSocket or IRouterSocket or IStreamSocket)
            events |= PollEventFlags.PollCompletion;
        source.Add(socket, events, Slot);
        return new ZLinkBackendSocketPoller(source);
    }

    internal static IZLinkBackendSocketPoller Create(ISocketMonitor monitor)
    {
        var source = ZLinkProcessExecutionPool.CreateReceiveSource();
        source.Add(monitor, PollEventFlags.PollIn, Slot);
        return new ZLinkBackendSocketPoller(source);
    }

    public ValueTask<ZLinkBackendSocketReadiness> WaitAsync(
        TimeSpan timeout,
        CancellationToken cancellationToken
    )
    {
        _source.SetCancellationToken(cancellationToken);
        _ = _source.WaitAsync(_events, timeout);
        return new ValueTask<ZLinkBackendSocketReadiness>(this, 0);
    }

    public ValueTaskSourceStatus GetStatus(short token) => _source.GetStatus(token);

    public void OnCompleted(
        Action<object?> continuation,
        object? state,
        short token,
        ValueTaskSourceOnCompletedFlags flags
    ) => _source.OnCompleted(continuation, state, token, flags);

    public ZLinkBackendSocketReadiness GetResult(short token) =>
        _source.GetResult(token) == 0
            ? ZLinkBackendSocketReadiness.None
            : MapReadiness(_events[0].Revents);

    public ValueTask ResumeAsync() => _source.ResumeAsync();

    public ValueTask DisposeAsync() => _source.DisposeAsync();

    private static ZLinkBackendSocketReadiness MapReadiness(PollEventFlags flags)
    {
        var readiness = ZLinkBackendSocketReadiness.None;
        if ((flags & PollEventFlags.PollIn) != 0)
            readiness |= ZLinkBackendSocketReadiness.Readable;
        if ((flags & PollEventFlags.PollOut) != 0)
            readiness |= ZLinkBackendSocketReadiness.Writable;
        if ((flags & PollEventFlags.PollErr) != 0)
            readiness |= ZLinkBackendSocketReadiness.Error;
        if ((flags & PollEventFlags.PollPri) != 0)
            readiness |= ZLinkBackendSocketReadiness.Priority;
        return readiness;
    }
}
