using MyGame.Shared;
using Zlink.Framework.Contracts.Channels;

namespace MyGame.Table;

public interface IPlayerRecords
{
    ValueTask<long> LoadPlayerAsync(string playerId, CancellationToken ct);
    ValueTask CommitRoundAsync(CommitRound round, CancellationToken ct);
}

internal sealed class ChannelPlayerRecords(IZLinkRouteClient client) : IPlayerRecords
{
    public async ValueTask<long> LoadPlayerAsync(string playerId, CancellationToken ct)
    {
        var player = await client
            .RequestToChannel(PlayerRecords.Channel, new LoadPlayer(playerId))
            .Async<PlayerRecord>(ct);
        return player.Chips;
    }

    public async ValueTask CommitRoundAsync(CommitRound round, CancellationToken ct) =>
        _ = await client.RequestToChannel(PlayerRecords.Channel, round).Async<RoundCommitted>(ct);
}
