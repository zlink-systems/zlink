// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>A reusable output container for one decoded STREAM packet.</summary>
public sealed partial class StreamPacket : IDisposable
{
    private StreamPacket()
    {
    }

    /// <summary>Creates an empty reusable packet output.</summary>
    public static StreamPacket Create()
    {
        return new StreamPacket();
    }

    /// <summary>Gets whether this output currently owns no packet.</summary>
    public bool IsEmpty => _header is null && _body is null;

    /// <summary>Gets the source connection routing id.</summary>
    public RoutingId? RoutingId => _routingId;

    /// <summary>Gets the decoded packet header.</summary>
    public Message? Header => _header;

    /// <summary>Gets the decoded packet body.</summary>
    public Message? Body => _body;

    /// <summary>Releases the current packet and leaves this output reusable.</summary>
    public void Dispose()
    {
        ResetForReuse();
    }
}
