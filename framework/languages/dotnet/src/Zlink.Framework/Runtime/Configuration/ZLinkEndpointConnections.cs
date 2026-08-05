namespace Zlink.Framework.Runtime.Configuration;

internal sealed class ZLinkEndpointConnections : IZLinkEndpointConnections, IReadOnlyCollection<string>
{
    private readonly List<string> _endpoints = [];
    private Attachment? _attachment;
    private ZLinkPeerAcquisitionMode? _frozenMode;

    public void Connect(string endpoint)
    {
        Validate(endpoint);
        Action<string>? connect;
        lock (_endpoints)
        {
            EnsureManualMutationAllowed();
            if (_endpoints.Contains(endpoint, StringComparer.Ordinal)) return;
            connect = _attachment?.Connect;
            connect?.Invoke(endpoint);
            _endpoints.Add(endpoint);
        }
    }

    public void Disconnect(string endpoint)
    {
        Validate(endpoint);
        lock (_endpoints)
        {
            EnsureManualMutationAllowed();
            var index = _endpoints.FindIndex(value => string.Equals(value, endpoint, StringComparison.Ordinal));
            if (index < 0) return;
            _attachment?.Disconnect(endpoint);
            _endpoints.RemoveAt(index);
        }
    }

    public IReadOnlyList<string> ListConnections()
    {
        lock (_endpoints) return _endpoints.ToArray();
    }

    internal IDisposable Attach(Action<string> connect, Action<string> disconnect)
    {
        ArgumentNullException.ThrowIfNull(connect);
        ArgumentNullException.ThrowIfNull(disconnect);
        lock (_endpoints)
        {
            // Framework registrations outlive one runtime generation. A
            // restart replaces the disposed generation's callbacks before
            // replaying the configured endpoint set.
            foreach (var endpoint in _endpoints) connect(endpoint);
            var attachment = new Attachment(this, connect, disconnect);
            _attachment = attachment;
            return attachment;
        }
    }

    private void Detach(Attachment attachment)
    {
        lock (_endpoints)
        {
            if (ReferenceEquals(_attachment, attachment))
                _attachment = null;
        }
    }

    internal void Freeze(ZLinkPeerAcquisitionMode mode)
    {
        lock (_endpoints)
        {
            if (_frozenMode is { } frozen && frozen != mode)
                throw new InvalidOperationException(
                    $"Connection acquisition mode is already frozen as '{frozen}'.");
            _frozenMode = mode;
        }
    }

    public int Count
    {
        get { lock (_endpoints) return _endpoints.Count; }
    }

    public IEnumerator<string> GetEnumerator() => ListConnections().GetEnumerator();

    System.Collections.IEnumerator System.Collections.IEnumerable.GetEnumerator() => GetEnumerator();

    private static void Validate(string endpoint)
    {
        if (string.IsNullOrWhiteSpace(endpoint))
            throw new ArgumentException("Endpoint must not be empty.", nameof(endpoint));
    }

    private void EnsureManualMutationAllowed()
    {
        if (_frozenMode == ZLinkPeerAcquisitionMode.AutoConnect)
            throw new InvalidOperationException(
                "Connections are managed by the location store for this role.");
    }

    private sealed class Attachment(
        ZLinkEndpointConnections owner,
        Action<string> connect,
        Action<string> disconnect) : IDisposable
    {
        private int _disposed;

        public Action<string> Connect { get; } = connect;

        public Action<string> Disconnect { get; } = disconnect;

        public void Dispose()
        {
            if (Interlocked.Exchange(ref _disposed, 1) == 0)
                owner.Detach(this);
        }
    }
}
