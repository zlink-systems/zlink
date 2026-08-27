using System.Collections.Concurrent;

namespace Zlink.Framework.Runtime.Host;

internal sealed class ZLinkActorBoundSessionRegistry(Action<string, string> unbind)
{
    private readonly ConcurrentDictionary<RoutingId, Entry[]> _entries = new();

    public void Register(
        string actorId,
        RoutingId sessionRid,
        string bindingToken)
    {
        if (!ZLinkActorBoundSessionBindingToken.IsNative(bindingToken)) return;

        while (true)
        {
            if (!_entries.TryGetValue(sessionRid, out var entries))
            {
                if (_entries.TryAdd(sessionRid, [new Entry(actorId, bindingToken)]))
                    return;

                continue;
            }

            var updated = entries
                .Where(entry => !entry.Matches(actorId, bindingToken))
                .Append(new Entry(actorId, bindingToken))
                .ToArray();
            if (_entries.TryUpdate(sessionRid, updated, entries))
                return;
        }
    }

    public void Unregister(
        string actorId,
        string bindingToken)
    {
        if (!ZLinkActorBoundSessionBindingToken.IsNative(bindingToken)) return;

        foreach (var (sessionRid, _) in _entries)
        {
            while (_entries.TryGetValue(sessionRid, out var entries))
            {
                var updated = entries
                    .Where(entry => !entry.Matches(actorId, bindingToken))
                    .ToArray();
                if (updated.Length == entries.Length)
                    break;

                if (updated.Length == 0)
                {
                    if (TryRemove(sessionRid, entries))
                        break;

                    continue;
                }

                if (_entries.TryUpdate(sessionRid, updated, entries))
                    break;
            }
        }
    }

    public void Cleanup(RoutingId sessionRid)
    {
        if (!_entries.TryRemove(sessionRid, out var entries)) return;

        foreach (var entry in entries)
            unbind(entry.ActorId, entry.BindingToken);
    }

    public void Clear()
    {
        _entries.Clear();
    }

    private bool TryRemove(RoutingId sessionRid, Entry[] entries)
    {
        return ((ICollection<KeyValuePair<RoutingId, Entry[]>>)_entries).Remove(
            new KeyValuePair<RoutingId, Entry[]>(sessionRid, entries));
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
