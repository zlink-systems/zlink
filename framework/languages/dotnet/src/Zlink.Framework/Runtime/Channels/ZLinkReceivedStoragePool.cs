using System.Collections.Concurrent;

namespace Zlink.Framework.Runtime.Channels;

// A Received instance owns native message parts until it is returned. The pool
// is used only where the receive loop can prove that no consumer retains the
// storage after dispatch or rejection has completed.
internal sealed class ZLinkReceivedStoragePool
{
    private readonly ConcurrentBag<Received> _available = new();

    internal Received Rent()
    {
        return _available.TryTake(out var storage)
            ? storage
            : Received.Create();
    }

    internal void Return(Received storage)
    {
        ArgumentNullException.ThrowIfNull(storage);
        storage.Dispose();
        _available.Add(storage);
    }
}
