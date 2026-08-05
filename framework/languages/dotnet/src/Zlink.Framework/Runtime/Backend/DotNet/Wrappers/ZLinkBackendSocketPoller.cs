using Zlink.Framework.Runtime.Backend.Contracts;

namespace Zlink.Framework.Runtime.Backend.DotNet.Wrappers;

// One Framework receive owner registers one native socket with one public ZLink
// poller. Receive readiness is always owned here. Request/response completion
// can also be registered here when the socket issues asynchronous requests, so
// the binding does not create a second progress owner for the same socket.
internal sealed class ZLinkBackendSocketPoller : IZLinkBackendSocketPoller
{
    private const nuint Slot = 1;
    private readonly IPoller _poller;
    private readonly PollEvent[] _events = new PollEvent[1];

    private ZLinkBackendSocketPoller(IPoller poller)
    {
        _poller = poller;
    }

    internal static IZLinkBackendSocketPoller Create(
        IZlinkSocket socket,
        bool includeRequestCompletion = false)
    {
        ArgumentNullException.ThrowIfNull(socket);
        var poller = Systems.Zlink.Zlink.CreatePoller();
        try
        {
            var events = PollEventFlags.PollIn;
            if (includeRequestCompletion)
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

    public PollEventFlags Wait(TimeSpan timeout)
    {
        return _poller.Wait(_events, timeout) == 0
            ? PollEventFlags.None
            : _events[0].Revents;
    }

    public void Dispose() => _poller.Dispose();
}
