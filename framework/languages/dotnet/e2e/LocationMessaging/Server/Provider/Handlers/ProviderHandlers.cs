using System.Security.Cryptography;
using System.Text;
using LocationMessaging.Server.Provider.Infrastructure;
using LocationMessaging.Shared;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Handlers;

namespace LocationMessaging.Server.Provider.Handlers;

internal sealed class ProfileRequestHandler(EvidenceStore evidence)
    : IZLinkRequestHandler<ProfileReq, ProfileRes>
{
    public async ValueTask<ProfileRes> HandleAsync(
        ProfileReq request,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        if (request.Value == "slow") await Task.Delay(TimeSpan.FromSeconds(1), cancellationToken);
        if (request.Value.StartsWith("rm-b3-transition-", StringComparison.Ordinal))
        {
            evidence.Add($"profile-request-start|rid={evidence.Rid}|value={request.Value}");
            await Task.Delay(TimeSpan.FromSeconds(1), cancellationToken);
        }

        evidence.Add($"profile-request|rid={evidence.Rid}|value={request.Value}|packet={context.PacketName}");
        return new ProfileRes($"profile:{request.Value}", evidence.Rid);
    }
}

internal sealed class ProfileCommandHandler(EvidenceStore evidence)
    : IZLinkSendHandler<ProfileMsg>
{
    public async ValueTask HandleAsync(
        ProfileMsg command,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (command.CommandId.StartsWith("rm-c9-slow-", StringComparison.Ordinal))
            await Task.Delay(TimeSpan.FromSeconds(1), cancellationToken);

        evidence.Add($"profile-command|rid={evidence.Rid}|command={command.CommandId}|packet={context.PacketName}");
    }
}

internal sealed class BackpressureCommandHandler(
    EvidenceStore evidence,
    BackpressureGate gate)
    : IZLinkSendHandler<BackpressureMsg>
{
    public async ValueTask HandleAsync(
        BackpressureMsg command,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        evidence.Add(
            $"profile-command-start|rid={evidence.Rid}|command={command.CommandId}"
            + $"|payloadBytes={command.Payload.Length}|packet={context.PacketName}");
        await gate.WaitAsync(cancellationToken);
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add(
            $"profile-command|rid={evidence.Rid}|command={command.CommandId}"
            + $"|payloadBytes={command.Payload.Length}|packet={context.PacketName}");
    }
}

internal sealed class PayloadRequestHandler(EvidenceStore evidence)
    : IZLinkRequestHandler<PayloadReq, PayloadRes>
{
    public ValueTask<PayloadRes> HandleAsync(
        PayloadReq request,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var hash = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(request.Payload)));
        evidence.Add(
            $"payload-request|rid={evidence.Rid}|marker={request.Marker}"
            + $"|length={request.Payload.Length}|sha256={hash}|packet={context.PacketName}");
        return ValueTask.FromResult(new PayloadRes(request.Marker, request.Payload.Length, hash));
    }
}

internal sealed class RoutePingHandler(EvidenceStore evidence)
    : IZLinkRouteRequestHandler<ScenarioRoutePing, ScenarioRoutePong>
{
    public ValueTask<ScenarioRoutePong> HandleAsync(
        ScenarioRoutePing request,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var source = context.SourceNodeRid.ToString();
        evidence.Add($"route-request|rid={evidence.Rid}|source={source}|value={request.Value}");
        return ValueTask.FromResult(new ScenarioRoutePong($"route:{request.Value}", evidence.Rid, source));
    }
}

internal sealed class ProfileMeshEventObserver(
    EvidenceStore evidence,
    IZLinkRouteMeshRuntime meshRuntime,
    IHostApplicationLifetime applicationLifetime)
    : BackgroundService
{
    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        // Hosted services may start before the framework runtime service.
        // Subscribe only after the host has completed its startup sequence.
        if (!applicationLifetime.ApplicationStarted.IsCancellationRequested)
        {
            var started = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);
            using var startedRegistration =
                applicationLifetime.ApplicationStarted.Register(
                    static state =>
                        ((TaskCompletionSource)state!).TrySetResult(),
                    started);
            using var stoppingRegistration = stoppingToken.Register(
                static state =>
                    ((TaskCompletionSource)state!).TrySetCanceled(),
                started);
            await started.Task.ConfigureAwait(false);
        }

        var previous = new HashSet<string>(StringComparer.Ordinal);
        await foreach (var status in meshRuntime
                           .ObserveAsync("profile", stoppingToken)
                           .ConfigureAwait(false))
        {
            var current = status.Status.Peers
                .Where(static peer => peer.State == ZLinkPeerState.Ready)
                .Select(static peer => peer.NodeRid.ToString())
                .ToHashSet(StringComparer.Ordinal);
            foreach (var peer in current.Except(previous))
                Add("ConnectionReady", peer, status.Status.Sequence);
            foreach (var peer in previous.Except(current))
                Add("Disconnected", peer, status.Status.Sequence);
            previous = current;
        }
    }

    private void Add(string kind, string peer, ulong sequence) =>
        evidence.Add(
            $"monitor-mesh|source=profile|kind={kind}"
            + $"|remote=|routing={peer}|sequence={sequence}");
}
