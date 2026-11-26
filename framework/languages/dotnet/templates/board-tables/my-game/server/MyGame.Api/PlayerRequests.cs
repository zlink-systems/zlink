using MyGame.Shared;
using Zlink.Framework.Contracts.Handlers;

namespace MyGame.Api;

public sealed class LoadPlayerHandler(RedisPlayerRecords records)
    : IZLinkRequestHandler<LoadPlayer, PlayerRecord>
{
    public async ValueTask<PlayerRecord> HandleAsync(
        LoadPlayer request,
        IZLinkMessageContext context,
        CancellationToken ct
    ) => new(request.PlayerId, await records.LoadPlayerAsync(request.PlayerId));
}

public sealed class CommitRoundHandler(RedisPlayerRecords records)
    : IZLinkRequestHandler<CommitRound, RoundCommitted>
{
    public async ValueTask<RoundCommitted> HandleAsync(
        CommitRound request,
        IZLinkMessageContext context,
        CancellationToken ct
    )
    {
        await records.CommitRoundAsync(request);
        return new RoundCommitted(request.TableId, request.HandNumber);
    }
}
