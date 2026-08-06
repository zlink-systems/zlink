using System.Collections.Concurrent;

namespace Zlink.Framework.Runtime.Channels;

// A TopicMessage owns its received parts until it is returned. The binding's
// public Subscribe contract accepts reusable caller-provided storage, so the
// pool keeps the envelope allocation out of the subscriber receive path while
// retaining each envelope until its asynchronous dispatch has finished.
internal sealed class ZLinkTopicMessageStoragePool
{
    private readonly ConcurrentBag<TopicMessage> _available = new();

    internal TopicMessage Rent()
    {
        return _available.TryTake(out var storage)
            ? storage
            : new TopicMessage();
    }

    internal void Return(TopicMessage storage)
    {
        ArgumentNullException.ThrowIfNull(storage);
        // Release message parts and metadata while retaining the binding's
        // reusable topic receive buffers. Dispose would drop those buffers and
        // make every next Subscribe allocate them again.
        storage.ReleaseForReuse();
        _available.Add(storage);
    }
}
