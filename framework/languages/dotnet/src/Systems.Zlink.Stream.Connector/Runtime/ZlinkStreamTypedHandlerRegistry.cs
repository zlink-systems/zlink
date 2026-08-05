namespace Systems.Zlink.Stream.Connector.Runtime;

internal sealed class ZlinkStreamTypedHandlerRegistry
{
    private readonly object _gate = new();
    private readonly Dictionary<string, TypedHandler[]> _handlers = new(StringComparer.Ordinal);

    public IDisposable Add(
        string name,
        Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, CancellationToken, ValueTask> handler)
    {
        var typed = new TypedHandler(handler);

        lock (_gate)
        {
            if (!_handlers.TryGetValue(name, out var handlers))
            {
                _handlers.Add(name, [typed]);
                return new Subscription(() => Remove(name, typed));
            }

            var next = new TypedHandler[handlers.Length + 1];
            Array.Copy(handlers, next, handlers.Length);
            next[^1] = typed;
            _handlers[name] = next;
        }

        return new Subscription(() => Remove(name, typed));
    }

    public IReadOnlyList<TypedHandler> Snapshot(string messageName)
    {
        lock (_gate)
        {
            return _handlers.TryGetValue(messageName, out var handlers)
                ? handlers
                : Array.Empty<TypedHandler>();
        }
    }

    private void Remove(string name, TypedHandler typed)
    {
        lock (_gate)
        {
            if (!_handlers.TryGetValue(name, out var handlers)) return;

            var index = Array.IndexOf(handlers, typed);
            if (index < 0) return;

            if (handlers.Length == 1)
            {
                _handlers.Remove(name);
                return;
            }

            var next = new TypedHandler[handlers.Length - 1];
            if (index > 0) Array.Copy(handlers, 0, next, 0, index);

            if (index < handlers.Length - 1)
                Array.Copy(
                    handlers,
                    index + 1,
                    next,
                    index,
                    handlers.Length - index - 1);

            _handlers[name] = next;
        }
    }

    internal sealed record TypedHandler(
        Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, CancellationToken, ValueTask> Invoke);

    private sealed class Subscription(Action dispose) : IDisposable
    {
        private int _disposed;

        public void Dispose()
        {
            if (Interlocked.Exchange(ref _disposed, 1) == 0) dispose();
        }
    }
}
