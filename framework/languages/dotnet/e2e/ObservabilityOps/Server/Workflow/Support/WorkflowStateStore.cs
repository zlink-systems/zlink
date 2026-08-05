using StackExchange.Redis;

namespace ObservabilityOps.Server.Workflow.Support;

internal sealed class WorkflowStateStore(IConnectionMultiplexer redis, WorkflowOptions options)
{
    public async Task<(int Version, string State)> LoadAsync(string workflowRid)
    {
        var values = await redis.GetDatabase().HashGetAsync(Key(workflowRid), ["version", "state"]);
        return (int.TryParse(values[0].ToString(), out var version) ? version : 0,
            values[1].HasValue ? values[1].ToString() : "created");
    }

    public Task SaveAsync(string workflowRid, int version, string state) =>
        redis.GetDatabase().HashSetAsync(Key(workflowRid),
            [new HashEntry("version", version), new HashEntry("state", state)]);

    private string Key(string workflowRid) => $"{options.RedisKeyPrefix}workflow-state:{workflowRid}";
}
