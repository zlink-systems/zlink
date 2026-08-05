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
