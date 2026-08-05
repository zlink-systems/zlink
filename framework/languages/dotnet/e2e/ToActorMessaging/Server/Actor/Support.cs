using System.Collections.Concurrent;
using ToActorMessaging.Shared;

namespace ToActorMessaging.Actor;

using Zlink.Framework.E2E.Configuration;

internal sealed record ServerOptions(
    string Rid,
    string HttpUrl,
    string RedisEndpoint,
    string RedisKeyPrefix,
    string RouterEndpoint,
    string PubSubEndpoint,
    string EvidenceFile,
    string LogDir)
{
    public static ServerOptions Parse(string[] args, string role)
        => E2eConfiguration.Load<ServerOptions>(args);
}

internal sealed class EvidenceStore(string path)
{
    private readonly ConcurrentQueue<ActorEvidence> _items = new();

    public void Append(ActorEvidence evidence)
    {
        _items.Enqueue(evidence);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.AppendAllLines(path,
        [
            $"{evidence.Scenario}|{evidence.ActorId}|{evidence.Kind}|{evidence.Value}"
            + $"|node={evidence.NodeRid ?? "<none>"}"
            + $"|generation={evidence.Generation?.ToString() ?? "<none>"}"
            + $"|packet={evidence.PacketName ?? "<none>"}"
            + $"|request={evidence.RequestId ?? "<none>"}"
        ]);
    }

    public IReadOnlyList<ActorEvidence> All() => _items.ToArray();
}
