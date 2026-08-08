namespace Zlink.Framework.Runtime.Host;

internal sealed class ZLinkActorBoundSessionRegistry(Action<string, string> unbind)
{
    private readonly Dictionary<RoutingId, List<Entry>> _entries = [];
    private readonly object _gate = new();

    public void Register(
        string actorId,
        RoutingId sessionRid,
        string bindingToken)
    {
        if (!ZLinkActorBoundSessionBindingToken.IsNative(bindingToken)) return;

        lock (_gate)
        {
            if (!_entries.TryGetValue(sessionRid, out var entries))
            {
                entries = [];
                _entries[sessionRid] = entries;
            }

            entries.RemoveAll(entry => entry.Matches(actorId, bindingToken));
            entries.Add(new Entry(actorId, bindingToken));
        }
    }

    public void Unregister(
        string actorId,
        string bindingToken)
    {
        if (!ZLinkActorBoundSessionBindingToken.IsNative(bindingToken)) return;

        lock (_gate)
        {
            foreach (var key in _entries.Keys.ToArray())
            {
                var entries = _entries[key];
                entries.RemoveAll(entry => entry.Matches(actorId, bindingToken));
                if (entries.Count == 0) _entries.Remove(key);
            }
        }
    }

    public void Cleanup(RoutingId sessionRid)
    {
        Entry[] entries;
        lock (_gate)
        {
            if (!_entries.Remove(sessionRid, out var registered)) return;

            entries = registered.ToArray();
        }

        foreach (var entry in entries)
            unbind(entry.ActorId, entry.BindingToken);
    }

    public void Clear()
    {
        lock (_gate)
        {
            _entries.Clear();
        }
    }

    private sealed record Entry(
        string ActorId,
        string BindingToken)
    {
        public bool Matches(
            string actorId,
            string bindingToken)
        {
            return string.Equals(ActorId, actorId, StringComparison.Ordinal)
                   && string.Equals(BindingToken, bindingToken, StringComparison.Ordinal);
        }
    }
}
