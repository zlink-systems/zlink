using System.Collections.Concurrent;
using SpotActorTransfer.Shared;

namespace SpotActorTransfer.SessionGateway;

using Zlink.Framework.E2E.Configuration;

internal sealed record GatewayOptions(
    string Rid,
    string HttpUrl,
    string RedisEndpoint,
    string RedisKeyPrefix,
    string RouterEndpoint,
    string StreamEndpoint,
    string EvidenceFile)
{
    public static GatewayOptions Parse(string[] args)
        => E2eConfiguration.Load<GatewayOptions>(args);
}

internal sealed class GatewayEvidenceStore(string nodeRid, string path)
{
    private readonly ConcurrentQueue<ActorEvidence> _items = new();

    public void Add(string scenario, string actorId, string kind, string value)
    {
        var item = new ActorEvidence(
            scenario,
            actorId,
            kind,
            value,
            nodeRid,
            DateTimeOffset.UtcNow.ToUnixTimeMilliseconds());
        _items.Enqueue(item);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.AppendAllLines(path, [$"{item.Scenario}|{item.ActorId}|{item.Kind}|{item.Value}|{item.NodeRid}"]);
    }
}
