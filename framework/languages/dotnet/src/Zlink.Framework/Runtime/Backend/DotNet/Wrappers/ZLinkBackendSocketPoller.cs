using Zlink.Framework.Runtime.Backend.Contracts;

namespace Zlink.Framework.Runtime.Backend.DotNet.Wrappers;

// One Framework receive owner waits on one native socket. Only receive
// readiness is owned here; asynchronous request progress remains inside the
// binding operation.
internal sealed class ZLinkBackendSocketPoller : IZLinkBackendSocketPoller
{
    private const nuint Slot = 1;
    private readonly IPoller _poller;
    private readonly PollEvent[] _events = new PollEvent[1];

    private ZLinkBackendSocketPoller(IPoller poller)
    {
        _poller = poller;
    }

    internal static IZLinkBackendSocketPoller Create(IZlinkSocket socket)
    {
        ArgumentNullException.ThrowIfNull(socket);
        if (socket is ISubSocket subscriber)
            return new ZLinkBackendSubscriberSocketPoller(subscriber);

        var poller = Systems.Zlink.Zlink.CreatePoller();
        try
        {
            poller.Add(
                socket,
                PollEventFlags.PollIn,
                Slot);
            return new ZLinkBackendSocketPoller(poller);
        }
        catch
        {
            poller.Dispose();
            throw;
        }
    }

    public ZLinkBackendSocketReadiness Wait(TimeSpan timeout)
    {
        return _poller.Wait(_events, timeout) == 0
            ? ZLinkBackendSocketReadiness.None
            : MapReadiness(_events[0].Revents);
    }

    public void Dispose() => _poller.Dispose();

    private static ZLinkBackendSocketReadiness MapReadiness(
        PollEventFlags flags)
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

    // SUB sockets have no send-completion channel. Use the binding's read-only
    // poll surface so receive readiness does not claim completion ownership.
    private sealed class ZLinkBackendSubscriberSocketPoller(
        ISubSocket subscriber) : IZLinkBackendSocketPoller
    {
        private readonly IZlinkSocket[] _sockets = [subscriber];
        private readonly PollEventFlags[] _requested = [PollEventFlags.PollIn];
        private readonly PollEventFlags[] _ready = new PollEventFlags[1];

        public ZLinkBackendSocketReadiness Wait(TimeSpan timeout)
        {
            _ready[0] = default;
            return ZlinkPoll.Poll(
                       _sockets,
                       _requested,
                       _ready,
                       ToTimeoutMilliseconds(timeout)) == 0
                ? ZLinkBackendSocketReadiness.None
                : MapReadiness(_ready[0]);
        }

        public void Dispose()
        {
        }

        private static int ToTimeoutMilliseconds(TimeSpan timeout)
        {
            if (timeout < TimeSpan.Zero)
                return -1;
            var milliseconds = Math.Ceiling(timeout.TotalMilliseconds);
            return milliseconds >= int.MaxValue
                ? int.MaxValue
                : (int)milliseconds;
        }
    }
}
