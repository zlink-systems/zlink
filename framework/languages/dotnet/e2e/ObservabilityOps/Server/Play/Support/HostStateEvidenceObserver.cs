using Microsoft.Extensions.Hosting;
using ObservabilityOps.Shared;
using Zlink.Framework.Contracts.Configuration;

namespace ObservabilityOps.Server.Play.Support;

/// <summary>
/// Records every host status the runtime publishes. Spec 24 §3 exposes state
/// changes as an observation stream precisely because intermediate states -
/// relocating among them - can pass faster than any snapshot poll, so a
/// scenario that needs to see one reads this evidence instead of racing it.
/// </summary>
internal sealed class HostStateEvidenceObserver(
    IZLinkFrameworkRuntime runtime,
    IZLinkRouteMeshRuntime meshRuntime,
    EvidenceStore evidence,
    string rid) : BackgroundService
{
    protected override Task ExecuteAsync(CancellationToken stoppingToken) =>
        Task.WhenAll(
            ObserveHostAsync(stoppingToken),
            ObserveMeshAsync(stoppingToken));

    private async Task ObserveHostAsync(CancellationToken stoppingToken)
    {
        try
        {
            await foreach (var status in runtime.ObserveAsync(stoppingToken))
                evidence.Add($"host-state|rid={rid}|state={status.Status.State}");
        }
        catch (OperationCanceledException)
        {
            // Host shutdown ends the observation; it owns no runtime decision.
        }
    }

    //  Peer draining is as short-lived as the host state it accompanies, so it
    //  is recorded from the same stream rather than polled.
    private async Task ObserveMeshAsync(CancellationToken stoppingToken)
    {
        try
        {
            await foreach (var status in meshRuntime.ObserveAsync(
                               ObservabilityNames.PlayMesh, stoppingToken))
                foreach (var peer in status.Status.Peers)
                    evidence.Add(
                        $"peer-state|rid={rid}|peer={peer.NodeRid}|state={peer.State}");
        }
        catch (OperationCanceledException)
        {
            // Same contract as the host stream.
        }
    }
}
