using System.Globalization;
using MyGame.Shared;
using StackExchange.Redis;

namespace MyGame.Api;

public sealed class RedisPlayerRecords(IConnectionMultiplexer connection, NodeSettings settings)
{
    private readonly IDatabase database = connection.GetDatabase();
    private readonly string prefix = settings.PlayerRecordsKeyPrefix ?? settings.KeyPrefix;
    private const string LoadPlayerScript = """
        local chips = redis.call('GET', KEYS[1])
        if not chips then
            redis.call('SET', KEYS[1], ARGV[1])
            return ARGV[1]
        end
        return chips
        """;

    // The marker and every player's balance are written atomically across API processes.
    private const string CommitRoundScript = """
        if redis.call('EXISTS', KEYS[1]) == 0 then
            local writes = {KEYS[1], '1'}
            for i = 2, #KEYS do
                writes[#writes + 1] = KEYS[i]
                writes[#writes + 1] = ARGV[i - 1]
            end
            redis.call('MSET', unpack(writes))
        end
        return 1
        """;

    public async ValueTask<long> LoadPlayerAsync(string playerId)
    {
        var chips = await database.ScriptEvaluateAsync(
            LoadPlayerScript,
            [PlayerKey(playerId)],
            [GameNames.InitialChips]
        );
        return long.Parse((string)chips!, CultureInfo.InvariantCulture);
    }

    public async ValueTask CommitRoundAsync(CommitRound round)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(round.TableId);
        if (
            round.HandNumber == 0
            || !GameNames.IsContractInteger(round.HandNumber)
            || round.Results is null
            || round.Results.Length is < GameNames.MinimumPlayers or > GameNames.SeatCount
        )
            throw new ArgumentException("Round identity and all seat balances are required.");

        var keys = new RedisKey[round.Results.Length + 1];
        var balances = new RedisValue[round.Results.Length];
        keys[0] =
            prefix
            + "rounds:"
            + round.HandNumber.ToString(CultureInfo.InvariantCulture)
            + ":"
            + round.TableId;
        var players = new HashSet<string>(StringComparer.Ordinal);
        for (var index = 0; index < round.Results.Length; index++)
        {
            var result = round.Results[index];
            if (
                result is null
                || !players.Add(result.PlayerId)
                || !GameNames.IsContractInteger(result.Chips)
            )
                throw new ArgumentException(
                    "Round results require unique player IDs and valid balances."
                );
            keys[index + 1] = PlayerKey(result.PlayerId);
            balances[index] = result.Chips;
        }
        await database.ScriptEvaluateAsync(CommitRoundScript, keys, balances);
    }

    private RedisKey PlayerKey(string playerId)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(playerId);
        return prefix + "players:" + playerId;
    }
}
