using StackExchange.Redis;
using ZoneWorld.Server.Ops.Ports;

namespace ZoneWorld.Server.Ops.Infrastructure.Store;

public sealed class MaintenanceStoreRepository(IConnectionMultiplexer redis, string keyPrefix)
    : IMaintenanceStorePort
{
    public async ValueTask WriteAsync(string nodeId, bool enabled, CancellationToken cancellationToken) =>
        await redis.GetDatabase().HashSetAsync($"{keyPrefix}maintenance", nodeId, enabled ? "1" : "0");
}
