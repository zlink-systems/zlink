// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     A received publish: its topic, source routing id, and message parts. Owns
///     its parts until disposed.
/// </summary>
public sealed partial class TopicMessage : IDisposable
{
    /// <summary>
    ///     Creates a topic message instance.
    /// </summary>
    public TopicMessage()
    {
    }

    /// <summary>
    ///     Gets the routing id.
    /// </summary>
    public RoutingId? RoutingId
    {
        get
        {
            return _routingIdSnapshot.GetOrCreateRoutingId(ref _routingId);
        }
    }

    /// <summary>
    ///     Decodes topic bytes to a topic string.
    /// </summary>
    public string Topic => _topic ??= DecodeTopicBytes();

    /// <summary>
    ///     Gets the parts.
    /// </summary>
    public IReadOnlyList<Message> Parts => PartsCollection;

    /// <summary>
    ///     Gets whether this publish carries exactly one part.
    /// </summary>
    public bool IsSinglePart => _singlePart != null || PartsCollection.IsSinglePart;

    /// <summary>
    ///     Releases resources owned by this instance.
    /// </summary>
    public void Dispose()
    {
        DisposeCore();
    }

    /// <summary>
    ///     Releases the message parts and metadata while keeping this instance
    ///     usable as caller-provided storage for a later <c>Subscribe</c> call.
    /// </summary>
    /// <remarks>
    ///     Call this only after consumers have finished using the current
    ///     message. Unlike <see cref="Dispose" />, this method retains the
    ///     internal topic receive buffers so repeated receives can reuse them.
    /// </remarks>
    public void ReleaseForReuse()
    {
        ResetForReuse();
    }

    /// <summary>
    ///     Returns the first message part; it stays owned by this envelope.
    /// </summary>
    public Message FirstPart()
    {
        return _singlePart
               ?? MessageEnvelopeParts.First(PartsCollection, nameof(TopicMessage));
    }

    /// <summary>
    ///     Returns the only message part, or throws when the publish is multipart;
    ///     the part stays owned by this envelope.
    /// </summary>
    public Message SinglePartOrThrow()
    {
        return _singlePart
               ?? MessageEnvelopeParts.Single(PartsCollection, nameof(TopicMessage));
    }
}
