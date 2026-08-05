using Systems.Zlink;

namespace Zlink.Framework.Runtime.Service;

// Private transport seam for the Framework service runtime. This type owns a
// raw ROUTER socket only; service state machines stay in the runtime owners that
// consume this port. Its public poller owns both receive readiness and request
// completion, so one native socket does not have two progress owners.
internal sealed class ZLinkRawRouterServicePort : IDisposable, IAsyncDisposable
{
    private readonly IRouterSocket _socket;
    private IPoller? _receivePoller;
    private readonly PollEvent[] _receiveEvents = new PollEvent[1];
    private bool _started;
    private bool _disposed;

    internal ZLinkRawRouterServicePort(
        IContext context,
        RoutingId routingId,
        string bindEndpoint)
    {
        ArgumentNullException.ThrowIfNull(context);
        if (routingId.IsEmpty)
            throw new ArgumentException("Routing id is required.", nameof(routingId));
        ArgumentException.ThrowIfNullOrWhiteSpace(bindEndpoint);

        RoutingId = routingId;
        BindEndpoint = bindEndpoint;
        _socket = context.CreateRouterSocket();
        _socket.Options.Mandatory = true;
        _socket.Options.Handover = true;
        _socket.SetRoutingId(routingId);
    }

    internal RoutingId RoutingId { get; }
    internal string BindEndpoint { get; }

    internal void Start()
    {
        ThrowIfDisposed();
        if (_started) return;
        _socket.Bind(BindEndpoint);
        var poller = Systems.Zlink.Zlink.CreatePoller();
        try
        {
            poller.Add(
                _socket,
                PollEventFlags.PollIn | PollEventFlags.PollCompletion,
                1);
            _receivePoller = poller;
        }
        catch
        {
            poller.Dispose();
            throw;
        }
        _started = true;
    }

    internal void Connect(string endpoint, RoutingId? expectedPeer = null)
    {
        EnsureStarted();
        ArgumentException.ThrowIfNullOrWhiteSpace(endpoint);
        if (expectedPeer is { } peer)
            _socket.Options.SetConnectRoutingId(peer);
        _socket.Connect(endpoint);
    }

    internal bool TrySend(
        RoutingId target,
        IReadOnlyList<ReadOnlyMemory<byte>> parts,
        SendFlags flags = SendFlags.None)
    {
        EnsureStarted();
        if (target.IsEmpty)
            throw new ArgumentException("Target routing id is required.", nameof(target));
        ArgumentNullException.ThrowIfNull(parts);
        if (parts.Count == 0)
            throw new ArgumentException("At least one message part is required.", nameof(parts));

        var messages = CreateMessages(parts);
        try
        {
            return _socket.Send(target).Messages(messages).Flags(flags).Submit();
        }
        finally
        {
            // A successful submit consumes each payload, but the managed Message
            // wrappers still require disposal. A failed submit restores payload
            // ownership, which is also released here because this port created it.
            foreach (var message in messages) message.Dispose();
        }
    }

    internal bool TryReceive(out ZLinkRawRouterEnvelope? envelope)
    {
        EnsureStarted();
        if (_receivePoller!.Wait(_receiveEvents, TimeSpan.Zero) == 0)
        {
            envelope = null;
            return false;
        }

        var readiness = _receiveEvents[0].Revents;
        if ((readiness & (PollEventFlags.PollIn
                          | PollEventFlags.PollErr
                          | PollEventFlags.PollPri)) == 0)
        {
            envelope = null;
            return false;
        }
        var received = Received.Create();
        try
        {
            if (_socket.Recv(received, RecvFlags.DontWait))
            {
                envelope = new ZLinkRawRouterEnvelope(received);
                return true;
            }

            received.Dispose();
            envelope = null;
            return false;
        }
        catch
        {
            received.Dispose();
            throw;
        }
    }

    internal async Task<ZLinkRawReplyEnvelope> RequestAsync(
        RoutingId target,
        IReadOnlyList<ReadOnlyMemory<byte>> parts,
        TimeSpan timeout,
        CancellationToken cancellationToken = default)
    {
        EnsureStarted();
        if (target.IsEmpty)
            throw new ArgumentException("Target routing id is required.", nameof(target));
        ArgumentNullException.ThrowIfNull(parts);
        if (parts.Count == 0)
            throw new ArgumentException("At least one message part is required.", nameof(parts));

        var messages = CreateMessages(parts);
        try
        {
            var reply = await _socket.Request(target)
                .Messages(messages)
                .Timeout(timeout)
                .Async(cancellationToken)
                .ConfigureAwait(false);
            return new ZLinkRawReplyEnvelope(reply);
        }
        finally
        {
            foreach (var message in messages) message.Dispose();
        }
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _receivePoller?.Dispose();
        _socket.Dispose();
    }

    public async ValueTask DisposeAsync()
    {
        if (_disposed) return;
        _disposed = true;
        _receivePoller?.Dispose();
        await _socket.DisposeAsync().ConfigureAwait(false);
    }

    private void EnsureStarted()
    {
        ThrowIfDisposed();
        if (!_started)
            throw new InvalidOperationException("The raw ROUTER port has not started.");
    }

    private void ThrowIfDisposed()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
    }

    internal static Message[] CreateMessages(
        IReadOnlyList<ReadOnlyMemory<byte>> parts)
    {
        var messages = new Message[parts.Count];
        var created = 0;
        try
        {
            for (; created < parts.Count; created++)
                messages[created] = Message.From(parts[created]);
            return messages;
        }
        catch
        {
            for (var index = 0; index < created; index++)
                messages[index].Dispose();
            throw;
        }
    }
}

internal sealed class ZLinkRawRouterEnvelope : IDisposable
{
    private readonly Received _received;

    internal ZLinkRawRouterEnvelope(Received received)
    {
        _received = received ?? throw new ArgumentNullException(nameof(received));
    }

    internal RoutingId SourceRoutingId =>
        _received.RoutingId
        ?? throw new InvalidOperationException("A ROUTER receive must include a source routing id.");

    internal IReadOnlyList<Message> Parts => _received.Parts;

    internal bool CanReply => _received.RequestSeq.HasValue;

    internal void Reply(IReadOnlyList<ReadOnlyMemory<byte>> parts)
    {
        if (!CanReply)
            throw new InvalidOperationException("The received envelope is not a request.");
        ArgumentNullException.ThrowIfNull(parts);
        if (parts.Count == 0)
            throw new ArgumentException("At least one reply part is required.", nameof(parts));

        var messages = ZLinkRawRouterServicePort.CreateMessages(parts);
        try
        {
            _received.Reply().Messages(messages).Submit();
        }
        finally
        {
            foreach (var message in messages) message.Dispose();
        }
    }

    public void Dispose() => _received.Dispose();
}

internal sealed class ZLinkRawReplyEnvelope : IDisposable
{
    private readonly IReadOnlyList<Message> _parts;

    internal ZLinkRawReplyEnvelope(IReadOnlyList<Message> parts)
    {
        _parts = parts ?? throw new ArgumentNullException(nameof(parts));
    }

    internal IReadOnlyList<Message> Parts => _parts;

    public void Dispose()
    {
        foreach (var part in _parts) part.Dispose();
    }
}
