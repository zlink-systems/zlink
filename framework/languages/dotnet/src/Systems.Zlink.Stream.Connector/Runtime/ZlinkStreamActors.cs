using System.Buffers.Binary;
using System.Text;

namespace Systems.Zlink.Stream.Connector.Runtime;

internal sealed class ZlinkStreamActors(
    IZlinkStreamConnectorInternal connector,
    ZlinkStreamConnectorCallbacks callbacks
)
{
    internal const string BoundControlName = "$zlink.actor.bound";
    internal const string UnboundControlName = "$zlink.actor.unbound";
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);
    private readonly object _gate = new();
    private readonly Dictionary<ushort, ZlinkStreamActor> _bySlot = [];
    private readonly Dictionary<string, ZlinkStreamActor> _byId = new(StringComparer.Ordinal);
    private readonly ZlinkStreamHandlerList<
        Func<IZlinkStreamActor, CancellationToken, ValueTask>
    > _boundHandlers = new();
    private readonly ZlinkStreamHandlerList<
        Func<IZlinkStreamActor, CancellationToken, ValueTask>
    > _unboundHandlers = new();

    internal IReadOnlyList<IZlinkStreamActor> Snapshot()
    {
        lock (_gate)
            return _bySlot.Values.Cast<IZlinkStreamActor>().ToArray();
    }

    internal IZlinkStreamActor? Find(string actorId)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(actorId);
        lock (_gate)
            return _byId.GetValueOrDefault(actorId);
    }

    internal IDisposable OnBound(Func<IZlinkStreamActor, CancellationToken, ValueTask> handler) =>
        _boundHandlers.Add(handler);

    internal IDisposable OnUnbound(Func<IZlinkStreamActor, CancellationToken, ValueTask> handler) =>
        _unboundHandlers.Add(handler);

    internal async ValueTask DispatchControlAsync(
        string name,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken
    )
    {
        if (name == BoundControlName)
        {
            var actor = Bind(payload.Span);
            await DispatchLifecycleAsync(_boundHandlers, actor, cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        if (name == UnboundControlName)
        {
            var actor = Unbind(payload.Span);
            await DispatchLifecycleAsync(_unboundHandlers, actor, cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        throw ZlinkStreamConnector.Error(
            ZlinkStreamErrorCode.FrameDecodeFailed,
            "Unknown Actor control packet."
        );
    }

    internal ZlinkStreamActor Resolve(ushort slot)
    {
        lock (_gate)
            if (_bySlot.TryGetValue(slot, out var actor))
                return actor;
        throw ZlinkStreamConnector.Error(
            ZlinkStreamErrorCode.FrameDecodeFailed,
            $"Actor slot '{slot}' is not bound."
        );
    }

    internal async ValueTask ConnectionEndedAsync()
    {
        ZlinkStreamActor[] actors;
        lock (_gate)
        {
            actors = _bySlot.Values.ToArray();
            _bySlot.Clear();
            _byId.Clear();
            foreach (var actor in actors)
                actor.Close();
        }

        foreach (var actor in actors)
            await DispatchLifecycleAsync(_unboundHandlers, actor, CancellationToken.None)
                .ConfigureAwait(false);
    }

    private ZlinkStreamActor Bind(ReadOnlySpan<byte> payload)
    {
        if (payload.Length < 5 || payload[0] != 1)
            throw DecodeError("Actor bound control payload is invalid.");
        var slot = BinaryPrimitives.ReadUInt16BigEndian(payload.Slice(1, 2));
        var idLength = payload[3];
        if (slot == 0 || idLength == 0 || payload.Length != 4 + idLength)
            throw DecodeError("Actor bound control payload is invalid.");

        string actorId;
        try
        {
            actorId = StrictUtf8.GetString(payload.Slice(4, idLength));
        }
        catch (DecoderFallbackException error)
        {
            throw DecodeError("Actor id is not valid UTF-8.", error);
        }

        lock (_gate)
        {
            if (_bySlot.ContainsKey(slot) || _byId.ContainsKey(actorId))
                throw DecodeError("Actor bound control duplicates an open binding.");
            var actor = new ZlinkStreamActor(connector, actorId, slot);
            _bySlot.Add(slot, actor);
            _byId.Add(actorId, actor);
            return actor;
        }
    }

    private ZlinkStreamActor Unbind(ReadOnlySpan<byte> payload)
    {
        if (payload.Length != 3 || payload[0] != 1)
            throw DecodeError("Actor unbound control payload is invalid.");
        var slot = BinaryPrimitives.ReadUInt16BigEndian(payload.Slice(1, 2));
        if (slot == 0)
            throw DecodeError("Actor unbound control payload is invalid.");

        lock (_gate)
        {
            if (!_bySlot.Remove(slot, out var actor))
                throw DecodeError("Actor unbound control names an unknown slot.");
            _byId.Remove(actor.ActorId);
            actor.Close();
            return actor;
        }
    }

    private async ValueTask DispatchLifecycleAsync(
        ZlinkStreamHandlerList<Func<IZlinkStreamActor, CancellationToken, ValueTask>> handlers,
        IZlinkStreamActor actor,
        CancellationToken cancellationToken
    )
    {
        foreach (var registration in handlers.Snapshot())
        {
            if (registration.IsRemoved)
                continue;
            await callbacks
                .DispatchUserCallbackAsync(
                    token => registration.Handler(actor, token),
                    cancellationToken
                )
                .ConfigureAwait(false);
        }
    }

    private static ZlinkStreamException DecodeError(string message, Exception? error = null) =>
        ZlinkStreamConnector.Error(ZlinkStreamErrorCode.FrameDecodeFailed, message, error);
}

internal interface IZlinkStreamActorRuntime : IZlinkStreamActor
{
    ZlinkStreamConnectorOptions Options { get; }
}

internal sealed class ZlinkStreamActor(
    IZlinkStreamConnectorInternal connector,
    string actorId,
    ushort slot
) : IZlinkStreamActorRuntime
{
    private readonly ZlinkStreamTypedHandlerRegistry _handlers = new();
    private int _bound = 1;

    public string ActorId { get; } = actorId;

    public bool IsBound => Volatile.Read(ref _bound) != 0;

    public ZlinkStreamConnectorOptions Options => connector.Options;

    internal ushort Slot => slot;

    public IZlinkStreamSendCall Send(ZlinkStreamEncodedPayload payload) =>
        new ZlinkStreamSendBuilder(connector, ResolveName(payload), payload, CurrentSlot);

    public IZlinkStreamRequestCall Request(ZlinkStreamEncodedPayload payload) =>
        new ZlinkStreamRequestBuilder(connector, ResolveName(payload), payload, CurrentSlot);

    public IDisposable On(
        string name,
        Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, CancellationToken, ValueTask> handler
    )
    {
        ArgumentNullException.ThrowIfNull(handler);
        ZlinkStreamConnector.ValidateName(name);
        return _handlers.Add(name, handler);
    }

    internal IReadOnlyList<ZlinkStreamTypedHandlerRegistry.TypedHandler> Handlers(string name) =>
        _handlers.Snapshot(name);

    internal void Close() => Volatile.Write(ref _bound, 0);

    private ushort? CurrentSlot()
    {
        if (!IsBound)
            throw ZlinkStreamConnector.Error(
                ZlinkStreamErrorCode.ValidationFailed,
                $"Actor '{ActorId}' is no longer bound."
            );
        return slot;
    }

    private string? ResolveName(ZlinkStreamEncodedPayload payload) =>
        payload.MessageType is { } type ? connector.Options.NameResolver.Resolve(type) : null;
}
