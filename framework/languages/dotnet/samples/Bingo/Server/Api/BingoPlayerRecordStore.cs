using System.Collections.Concurrent;
using Bingo.Shared.Contracts;

namespace Bingo.Server.Api;

internal sealed class BingoPlayerRecordStore
{
    private readonly ConcurrentDictionary<string, PlayerRecord> _records = new(StringComparer.Ordinal);

    public GetPlayerRecordRes Get(string actorId)
    {
        var record = _records.GetOrAdd(actorId, static id => new PlayerRecord(id, 0, 0));
        return new GetPlayerRecordRes
        {
            ActorId = record.ActorId,
            Wins = record.Wins,
            Losses = record.Losses
        };
    }

    public ReportBingoResultRes Report(string actorId, bool won)
    {
        var record = _records.AddOrUpdate(
            actorId,
            static (id, didWin) => didWin ? new PlayerRecord(id, 1, 0) : new PlayerRecord(id, 0, 1),
            static (_, current, didWin) => didWin
                ? current with { Wins = current.Wins + 1 }
                : current with { Losses = current.Losses + 1 },
            won);
        return new ReportBingoResultRes
        {
            ActorId = record.ActorId,
            Wins = record.Wins,
            Losses = record.Losses
        };
    }

    private sealed record PlayerRecord(string ActorId, int Wins, int Losses);
}
