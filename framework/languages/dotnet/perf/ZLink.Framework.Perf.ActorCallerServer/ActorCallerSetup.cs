using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Configuration;

namespace ZLink.Framework.Perf;

// §10.9 and §10.10 preparation: one unbound Actor per logical stream, created through the public manager during
// setup (§4). The create is never retried; only the public RouteMesh status says when the Actor node is a ready peer.
public sealed class ActorCallerSetup(Measurement measurement, IZLinkActorManager actors, IZLinkRouteMeshRuntime mesh)
{
    private readonly RoleConfig config = measurement.Config;

    // Returns the create result as evidence; the caller reports objectsReady once its probes have also finished.
    public async Task<object> CreateActorsAsync(CancellationToken cancellationToken)
    {
        while (mesh.GetStatus(config.meshName!).ReadyPeerCount == 0) await Task.Delay(10, cancellationToken);
        using var concurrency = new SemaphoreSlim(config.workload.connectConcurrency!.Value);
        long created = 0, existing = 0, totalNs = 0, maxNs = 0;
        await Task.WhenAll(config.actorIds.Select(async actorId =>
        {
            await concurrency.WaitAsync(cancellationToken);
            try
            {
                var started = PerfClock.Now;
                var result = await actors.GetOrCreate(actorId, PerfActorType.Name).InMesh(config.meshName!)
                    .Timeout(TimeSpan.FromMilliseconds(config.workload.setupTimeoutMs)).Async(cancellationToken);
                var elapsed = PerfClock.Now - started;
                if (result is ZLinkActorCreateResult.Created) Interlocked.Increment(ref created);
                else if (result is ZLinkActorCreateResult.Existing) Interlocked.Increment(ref existing);
                else throw new InvalidOperationException($"Actor '{actorId}' creation was rejected.");
                Interlocked.Add(ref totalNs, elapsed);
                long seen;
                while (elapsed > (seen = Interlocked.Read(ref maxNs)) && Interlocked.CompareExchange(ref maxNs, elapsed, seen) != seen) { }
            }
            finally { concurrency.Release(); }
        }));
        return new { kind = "actorCreate", source = "IZLinkActorManager.GetOrCreate.InMesh.Async",
            observedValue = new { created, existing, expectedActors = config.actorIds.Length,
                createMeanMs = totalNs / 1e6 / config.actorIds.Length, createMaxMs = maxNs / 1e6 } };
    }
}
