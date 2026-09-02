// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

public sealed partial class StreamPacket
{
    private RoutingId? _routingId;
    private Message? _header;
    private Message? _body;
    private int _receiving;

    internal void BeginReceive()
    {
        if (Interlocked.CompareExchange(ref _receiving, 1, 0) != 0)
            throw new InvalidOperationException(
                "The same StreamPacket cannot be used by concurrent receives.");
        ResetPayload();
    }

    internal void CompleteReceive(RoutingId routingId, Message header,
        Message body)
    {
        _routingId = routingId;
        _header = header;
        _body = body;
    }

    internal void EndReceive()
    {
        Volatile.Write(ref _receiving, 0);
    }

    private void ResetForReuse()
    {
        if (Volatile.Read(ref _receiving) != 0)
            throw new InvalidOperationException(
                "A StreamPacket receive is currently in progress.");
        ResetPayload();
    }

    private void ResetPayload()
    {
        _routingId = null;
        _header?.Dispose();
        _header = null;
        _body?.Dispose();
        _body = null;
    }
}
