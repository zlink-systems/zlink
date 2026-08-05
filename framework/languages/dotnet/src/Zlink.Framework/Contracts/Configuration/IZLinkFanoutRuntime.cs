namespace Zlink.Framework.Contracts.Configuration;

public sealed record ZLinkFanoutStatus(
    string ChannelName,
    ZLinkTopologyState State,
    bool IsReady,
    int ReadyPublisherCount,
    IReadOnlyList<ZLinkPeerStatus> Publishers,
    ulong Sequence,
    DateTimeOffset ObservedAt);

public interface IZLinkFanoutRuntime
{
    ZLinkFanoutStatus GetStatus(string channelName);

    IAsyncEnumerable<ZLinkObservedStatus<ZLinkFanoutStatus>> ObserveAsync(
        string channelName,
        CancellationToken cancellationToken = default);
}
