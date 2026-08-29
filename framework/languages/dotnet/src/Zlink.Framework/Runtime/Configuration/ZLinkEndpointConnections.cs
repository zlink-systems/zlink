using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Configuration;

internal sealed class ZLinkEndpointConnections : IZLinkEndpointConnections, IReadOnlyCollection<string>
{
    private readonly ZLinkStateLane _lane = new();
    private readonly List<string> _endpoints = [];
    private Attachment? _attachment;
    private ZLinkPeerAcquisitionMode? _frozenMode;

    public void Connect(string endpoint)
    {
        Validate(endpoint);
        // A caller may write the same target in a different (but equivalent)
        // notation than an earlier Connect or than the transport's own
        // canonical form; normalize once here so the set membership check
        // below and the attachment callback agree with every other write
        // point.
        var normalized = ZLinkEndpointNotation.Normalize(endpoint);
        AwaitStateLane(ConnectAsync(normalized));
    }

    private async ValueTask ConnectAsync(string normalized)
    {
        var prepared = await _lane.RunAsync(() =>
        {
            EnsureManualMutationAllowed();
            if (_endpoints.Contains(normalized, StringComparer.Ordinal)) return null;
            _endpoints.Add(normalized);
            return new EndpointCallback(_attachment?.Connect, _endpoints.Count - 1);
        }).ConfigureAwait(false);
        try
        {
            prepared?.Callback?.Invoke(normalized);
        }
        catch
        {
            if (prepared is not null)
            {
                await _lane.RunAsync(() =>
                {
                    if (_endpoints.Count > prepared.Index
                        && string.Equals(
                            _endpoints[prepared.Index],
                            normalized,
                            StringComparison.Ordinal))
                        _endpoints.RemoveAt(prepared.Index);
                }).ConfigureAwait(false);
            }
            throw;
        }
    }

    public void Disconnect(string endpoint)
    {
        Validate(endpoint);
        var normalized = ZLinkEndpointNotation.Normalize(endpoint);
        AwaitStateLane(DisconnectAsync(normalized));
    }

    private async ValueTask DisconnectAsync(string normalized)
    {
        var prepared = await _lane.RunAsync(() =>
        {
            EnsureManualMutationAllowed();
            var index = _endpoints.FindIndex(value => string.Equals(value, normalized, StringComparison.Ordinal));
            if (index < 0) return null;
            _endpoints.RemoveAt(index);
            return new EndpointCallback(_attachment?.Disconnect, index);
        }).ConfigureAwait(false);
        try
        {
            prepared?.Callback?.Invoke(normalized);
        }
        catch
        {
            if (prepared is not null)
            {
                await _lane.RunAsync(() =>
                {
                    if (!_endpoints.Contains(normalized, StringComparer.Ordinal))
                        _endpoints.Insert(
                            Math.Min(prepared.Index, _endpoints.Count),
                            normalized);
                }).ConfigureAwait(false);
            }
            throw;
        }
    }

    public IReadOnlyList<string> ListConnections()
    {
        return AwaitStateLane(_lane.RunAsync(() => _endpoints.ToArray()));
    }

    internal IDisposable Attach(Action<string> connect, Action<string> disconnect)
    {
        ArgumentNullException.ThrowIfNull(connect);
        ArgumentNullException.ThrowIfNull(disconnect);
        var prepared = AwaitStateLane(_lane.RunAsync(() =>
        {
            // Framework registrations outlive one runtime generation. A
            // restart replaces the disposed generation's callbacks before
            // replaying the configured endpoint set.
            var attachment = new Attachment(this, connect, disconnect);
            var previous = _attachment;
            _attachment = attachment;
            return new AttachmentPreparation(
                attachment,
                previous,
                _endpoints.ToArray());
        }));
        try
        {
            foreach (var endpoint in prepared.Endpoints)
                connect(endpoint);
        }
        catch
        {
            AwaitStateLane(_lane.RunAsync(() =>
            {
                if (ReferenceEquals(_attachment, prepared.Attachment))
                    _attachment = prepared.Previous;
            }));
            throw;
        }
        return prepared.Attachment;
    }

    private void Detach(Attachment attachment)
    {
        AwaitStateLane(_lane.RunAsync(() =>
        {
            if (ReferenceEquals(_attachment, attachment))
                _attachment = null;
        }));
    }

    internal void Freeze(ZLinkPeerAcquisitionMode mode)
    {
        AwaitStateLane(_lane.RunAsync(() =>
        {
            if (_frozenMode is { } frozen && frozen != mode)
                throw new InvalidOperationException(
                    $"Connection acquisition mode is already frozen as '{frozen}'.");
            _frozenMode = mode;
        }));
    }

    public int Count
    {
        get { return AwaitStateLane(_lane.RunAsync(() => _endpoints.Count)); }
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

    private static void AwaitStateLane(ValueTask operation) =>
        operation.GetAwaiter().GetResult();

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();

    private sealed record AttachmentPreparation(
        Attachment Attachment,
        Attachment? Previous,
        IReadOnlyList<string> Endpoints);

    private sealed record EndpointCallback(Action<string>? Callback, int Index);

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
