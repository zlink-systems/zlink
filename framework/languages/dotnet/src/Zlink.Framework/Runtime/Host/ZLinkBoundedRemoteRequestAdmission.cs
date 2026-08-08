using Zlink.Framework.Runtime.Actors;

namespace Zlink.Framework.Runtime.Host;

internal sealed class ZLinkBoundedRemoteRequestAdmission(
    int maxTotal = 65_536,
    int maxPerBinding = 1_024)
{
    private readonly Dictionary<ZLinkSessionBindingKey, int> _counts = [];
    private int _total;

    internal bool TryAcquire(ZLinkSessionBindingKey key)
    {
        _counts.TryGetValue(key, out var bindingCount);
        if (_total >= maxTotal || bindingCount >= maxPerBinding)
            return false;
        _counts[key] = bindingCount + 1;
        _total++;
        return true;
    }

    internal void Release(ZLinkSessionBindingKey key)
    {
        if (!_counts.TryGetValue(key, out var bindingCount))
            throw new InvalidOperationException(
                "Remote request admission was released without ownership.");
        if (bindingCount == 1)
            _counts.Remove(key);
        else
            _counts[key] = bindingCount - 1;
        _total--;
    }

    internal void Clear()
    {
        _counts.Clear();
        _total = 0;
    }
}
