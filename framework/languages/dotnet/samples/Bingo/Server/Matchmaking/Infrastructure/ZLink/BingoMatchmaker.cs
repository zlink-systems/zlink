using Zlink.Framework.Contracts.Spots;

namespace Bingo.Server.Matchmaking.Infrastructure.ZLink;

internal sealed class BingoMatchmaker(IZLinkInstanceSpotContext context)
    : IZLinkInstanceSpot
{
    public IZLinkInstanceSpotContext Context { get; } = context;

    internal DateTimeOffset LastActivity { get; private set; } = DateTimeOffset.UtcNow;

    internal void RecordActivity()
    {
        LastActivity = DateTimeOffset.UtcNow;
    }

    public async ValueTask OnInitializeAsync(CancellationToken cancellationToken)
    {
        await Context.AddTimer<BingoMatchmakerIdleTimer>(
            "bingo-matchmaker-idle",
            TimeSpan.FromSeconds(10),
            cancellationToken: cancellationToken);
    }
}
