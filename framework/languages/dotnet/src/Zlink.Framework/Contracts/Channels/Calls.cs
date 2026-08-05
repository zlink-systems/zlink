namespace Zlink.Framework.Contracts.Channels;

using Zlink.Framework.Contracts.Streams;

/// <summary>
///     Shared metadata builder of every send/request/publish call
///     (05-route-mesh §6). The last value set for a key wins; the encoded
///     frame is capped at 1024 bytes at submit time.
/// </summary>
public interface IZLinkMetadataCall<TSelf>
{
    TSelf Metadata(string key, string value);

    TSelf Metadata(ZLinkMessageMetadata metadata);
}

public interface IZLinkSendCall : IZLinkMetadataCall<IZLinkSendCall>
{
    ValueTask Async(
        CancellationToken cancellationToken = default);
}

public interface IZLinkRequestCall : IZLinkMetadataCall<IZLinkRequestCall>
{
    IZLinkRequestCall Timeout(TimeSpan timeout);

    ValueTask<TReply> Async<TReply>(CancellationToken cancellationToken = default);

    ValueTask<TReply> Yield<TReply>(CancellationToken cancellationToken = default);
}

public interface IZLinkPublishCall : IZLinkMetadataCall<IZLinkPublishCall>
{
    /// <summary>
    ///     Submits once to each selected remote MeshNode's local transport
    ///     queue and to each matching local Spot queue. Completion does not
    ///     wait for a remote Spot queue, subscriber handler, or remote ACK.
    /// </summary>
    ValueTask Async(
        CancellationToken cancellationToken = default);
}

public interface IZLinkFanoutPublishCall
{
    ValueTask Async(
        CancellationToken cancellationToken = default);
}
