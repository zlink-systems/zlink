using StackExchange.Redis;
using ZoneWorld.Server.ZoneNode.Ports;

namespace ZoneWorld.Server.ZoneNode.Infrastructure.Store;

/// <summary>
/// The operator's desired maintenance state, held in the same Redis the location store
/// uses. It is application state, not a ZLink surface: the framework has no opinion about
/// it, and it has to survive a node restart, which a fanout cannot promise (§8.4).
/// </summary>
public sealed class MaintenanceStoreRepository(IConnectionMultiplexer redis, string keyPrefix)
    : IMaintenanceStorePort
{
    private string Key => $"{keyPrefix}maintenance";

    public async ValueTask<IReadOnlyDictionary<string, bool>> ReadAllAsync(
        CancellationToken cancellationToken)
    {
        var entries = await redis.GetDatabase().HashGetAllAsync(Key);
        return entries.ToDictionary(
            entry => entry.Name.ToString(),
            entry => entry.Value == "1",
            StringComparer.Ordinal);
    }
}
