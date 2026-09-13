using Zlink.Framework.Runtime.Backend.Contracts;

namespace Zlink.Framework.Runtime.Backend.DotNet.Wrappers;

// One Framework receive owner registers one native socket with one public ZLink
// poller. Completion-capable sockets use that same poller as their sole async
// completion owner.
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
        var poller = Systems.Zlink.Zlink.CreatePoller();
        try
        {
            var events = PollEventFlags.PollIn;
            if (socket is IPairSocket or IDealerSocket or IRouterSocket
                or IStreamSocket)
                events |= PollEventFlags.PollCompletion;
            poller.Add(
                socket,
                events,
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
}
