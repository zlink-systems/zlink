// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     Describes the envelope kind of a received message.
/// </summary>
public enum ReceivedMessageType
{
    /// <summary>
    ///     A plain message with no request/reply framing.
    /// </summary>
    Raw = 0,

    /// <summary>
    ///     A request that can be replied to.
    /// </summary>
    Request = 1,

    /// <summary>
    ///     A successful reply to a request.
    /// </summary>
    Reply = 2,

    /// <summary>
    ///     An error reply to a request.
    /// </summary>
    ErrorReply = 3
}

/// <summary>
///     Holds one received message envelope.
/// </summary>
/// <remarks>
///     A received envelope owns its message parts until disposed or until an API
///     explicitly transfers ownership. Reuse instances created by <c>Create</c>
///     with receive APIs that accept caller-provided storage.
/// </remarks>
public sealed partial class Received : IDisposable
{
    /// <summary>
    ///     Gets the source routing id when the receive path provides one.
    /// </summary>
    public RoutingId? RoutingId
    {
        get
        {
            return _routingIdSnapshot.GetOrCreateRoutingId(ref _routingId);
        }
    }

    /// <summary>
    ///     Gets the request sequence when this envelope can be replied to.
    /// </summary>
    public ulong? RequestSeq => _metadata?.RequestSeq;

    /// <summary>
    ///     Gets the envelope kind.
    /// </summary>
    public ReceivedMessageType MessageType { get; private set; } =
        ReceivedMessageType.Raw;

    /// <summary>
    ///     Gets the message parts owned by this envelope.
    /// </summary>
    /// <remarks>
    ///     The returned messages are disposed when this envelope is disposed unless
    ///     ownership has been explicitly transferred by another API.
    /// </remarks>
    public IReadOnlyList<Message> Parts => PartsCollection;

    /// <summary>
    ///     Gets whether the envelope currently contains exactly one message part.
    /// </summary>
    public bool IsSinglePart => _singlePart != null || PartsCollection.IsSinglePart;

    /// <summary>
    ///     Disposes the message parts owned by this envelope.
    /// </summary>
    public void Dispose()
    {
        DisposeCore();
    }

    /// <summary>
    ///     Create an empty <see cref="Received" /> for caller-provided storage.
    ///     Hand the same instance to <c>Recv(Received, ...)</c> across calls to
    ///     avoid the per-recv allocation; the binding overwrites the internal
    ///     state on each successful receive.
    /// </summary>
    public static Received Create()
    {
        return new Received();
    }

    /// <summary>
    ///     Returns the first message part without transferring ownership.
    /// </summary>
    public Message FirstPart()
    {
        return _singlePart
               ?? MessageEnvelopeParts.First(PartsCollection, nameof(Received));
    }

    /// <summary>
    ///     Returns the only message part or throws when the envelope is multipart.
    /// </summary>
    public Message SinglePartOrThrow()
    {
        return _singlePart
               ?? MessageEnvelopeParts.Single(PartsCollection, nameof(Received));
    }

    /// <summary>
    ///     Start a reply operation for request envelopes.
    /// </summary>
    /// <remarks>
    ///     The operation is valid only when <see cref="RequestSeq" /> has a value.
    /// </remarks>
    public ReplyOperation Reply()
    {
        return new ReceivedReplyOperationImpl(CaptureReplyHandler());
    }

    /// <summary>
    ///     Start a send operation addressed to the source route of this envelope.
    /// </summary>
    public SendOperation Send()
    {
        return new ReceivedSendOperationImpl(this);
    }
}
