using SpotService.Shared;
using Systems.Zlink;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Spots;

using Zlink.Framework.Contracts.Locations;

namespace SpotService.Server.Play.Endpoints;

using static PlayHostFactory;

internal static class SpotInteractionEndpoints
{
    public static void MapSpotInteractionEndpoints(WebApplication app)
    {
        app.MapPost("/spot/outbound", async (
            IZLinkSpotClient routes,
            EvidenceStore evidence,
            NodeOptions node,
            SpotOutboundRouteReq request) =>
        {
            var before = evidence.Snapshot();
            await SendSpotCommandAsync(
                routes,
                request.SpotRid,
                new SpotOutboundMsg(request.Marker));
            await WaitUntilAsync(
                () =>
                {
                    var after = evidence.Snapshot();
                    return CountNew(after, before,
                               $"spot-outbound|rid={node.Rid}|spot={request.SpotRid}|echo=echo-sm-c2|notify=notify-sm-c2") >=
                           1
                           && CountNew(after, before,
                               $"spot-msg|rid={node.Rid}|spot={request.SpotRid}|marker=sm-c2-publish") >= 1
                           && CountNew(after, before, "channel-echo|value=sm-c2") >= 1
                           && CountNew(after, before, "channel-notify|marker=notify-sm-c2") >= 1;
                },
                "Expected spot outbound evidence.");
            return Results.Ok(new SpotOutboundRouteRes(
                request.SpotRid,
                request.Marker,
                true,
                evidence.Snapshot()));
        });
        app.MapPost("/spot/outbound-negative", async (
            IZLinkSpotClient routes,
            EvidenceStore evidence,
            NodeOptions node,
            SpotOutboundRouteReq request) =>
        {
            var before = evidence.Snapshot();
            await SendSpotCommandAsync(
                routes,
                request.SpotRid,
                new SpotOutboundNegativeMsg(request.Marker));
            await WaitUntilAsync(
                () =>
                {
                    var after = evidence.Snapshot();
                    return CountNew(after, before,
                               $"spot-outbound-negative|rid={node.Rid}|spot={request.SpotRid}|requestFailed=True") >= 1
                           && CountNew(after, before,
                               "dispatch-error|surface=Channel|reason=HandlerMissing|action=ReplyError|packet=MissingChannelReq") >=
                           1
                           && CountNew(after, before,
                               "dispatch-error|surface=Channel|reason=HandlerMissing|action=Drop|packet=MissingChannelNotify") >=
                           1;
                },
                "Expected spot outbound negative evidence.");
            return Results.Ok(new SpotOutboundRouteRes(
                request.SpotRid,
                request.Marker,
                true,
                evidence.Snapshot()));
        });
        app.MapPost("/spot/stage/request", async (
            IZLinkSpotClient routes,
            SpotStageProbeReq request) =>
        {
            var result = await routes.RequestToSpot(request.SpotRid,
                    new StageProbeReq(request.Marker, request.Delta))
                .Timeout(TimeSpan.FromSeconds(5))
                .Async<StateRes>();
            return Results.Ok(result);
        });
        app.MapPost("/spot/stage/timer", async (
            IZLinkSpotClient routes,
            EvidenceStore evidence,
            NodeOptions node,
            SpotStageTimerReq request) =>
        {
            var before = evidence.Snapshot();
            await SendSpotCommandAsync(
                routes,
                request.SpotRid,
                new StageTimerStartMsg(request.Name, request.PeriodMs));
            await WaitUntilAsync(
                () => CountNew(evidence.Snapshot(), before,
                    $"stage-timer|rid={node.Rid}|spot={request.SpotRid}|name={request.Name}") >= 1,
                "Expected spot stage timer evidence.");
            return Results.Ok(new SpotStageTimerRes(
                request.SpotRid,
                request.Name,
                true,
                evidence.Snapshot()));
        });
        app.MapPost("/spot/timer/start", async (
            IZLinkSpotClient routes,
            EvidenceStore evidence,
            NodeOptions node,
            SpotTimerStartReq request) =>
        {
            var before = evidence.Snapshot();
            await SendSpotCommandAsync(
                routes,
                request.SpotRid,
                new TimerStartMsg(request.Name, request.PeriodMs));
            await WaitUntilAsync(
                () => CountNew(evidence.Snapshot(), before,
                    $"timer-basic|rid={node.Rid}|spot={request.SpotRid}|name={request.Name}") >= 2,
                "Expected repeated spot timer evidence.");
            return Results.Ok(new SpotTimerStartRes(
                request.SpotRid,
                request.Name,
                true,
                evidence.Snapshot()));
        });
        app.MapPost("/spot/idle-close/start", async (
            IZLinkSpotClient routes,
            EvidenceStore evidence,
            NodeOptions node,
            SpotIdleCloseReq request) =>
        {
            var before = evidence.Snapshot();
            await SendSpotCommandAsync(
                routes,
                request.SpotRid,
                new IdleCloseMsg(request.Name, request.PeriodMs));
            await WaitUntilAsync(
                () =>
                {
                    var after = evidence.Snapshot();
                    return CountNew(after, before,
                               $"timer-idle-close|rid={node.Rid}|spot={request.SpotRid}|name={request.Name}|closed=True") ==
                           1
                           && CountNew(after, before, $"spot-closing|rid={node.Rid}|spot={request.SpotRid}") == 1;
                },
                "Expected spot idle close evidence.");
            return Results.Ok(new SpotIdleCloseRes(
                request.SpotRid,
                request.Name,
                true,
                evidence.Snapshot()));
        });
        app.MapPost("/spot/overrun/start", async (
            IZLinkSpotClient routes,
            EvidenceStore evidence,
            NodeOptions node,
            SpotOverrunStartReq request) =>
        {
            var before = evidence.Snapshot();
            await SendSpotCommandAsync(
                routes,
                request.SpotRid,
                new OverrunStartMsg(request.Name, request.Policy, request.PeriodMs));
            await WaitUntilAsync(
                () => CountNew(evidence.Snapshot(), before,
                    $"timer-overrun|rid={node.Rid}|spot={request.SpotRid}|name={request.Name}") >= 3,
                "Expected spot overrun timer evidence.");
            return Results.Ok(new SpotOverrunStartRes(
                request.SpotRid,
                request.Name,
                request.Policy,
                true,
                evidence.Snapshot()));
        });
        app.MapPost("/spot/worker/start", async (
            IZLinkSpotClient routes,
            SpotWorkerStartReq request) =>
        {
            var result = await routes.RequestToSpot(request.SpotRid,
                    new WorkerStartReq(request.Marker, request.DelayMs))
                .Timeout(TimeSpan.FromSeconds(30))
                .Async<WorkerStartRes>();
            return Results.Ok(result);
        });
        app.MapPost("/spot/worker/complete", async (
            EvidenceStore evidence,
            NodeOptions node,
            SpotWorkerCompleteReq request) =>
        {
            await WaitUntilAsync(
                () => evidence.Snapshot().Any(line =>
                    line.Contains($"worker-complete|rid={node.Rid}|spot={request.SpotRid}|marker={request.Marker}",
                        StringComparison.Ordinal)),
                "Expected worker completion evidence.");
            return Results.Ok(new SpotWorkerCompleteRes(
                request.SpotRid,
                request.Marker,
                true,
                evidence.Snapshot()));
        });
        app.MapPost("/spot/to-spot/request", async (
            IZLinkSpotClient routes,
            EvidenceStore evidence,
            NodeOptions node,
            SpotToSpotRouteReq request) =>
        {
            var result = await RequestSpotToSpotAsync(
                routes,
                request.SourceSpotRid,
                new SpotToSpotReq(request.TargetSpotRid, request.Marker));
            await WaitUntilAsync(
                () =>
                {
                    var after = evidence.Snapshot();
                    return CountNew(after, [],
                               $"spot-to-spot|rid={node.Rid}|source={request.SourceSpotRid}|target={request.TargetSpotRid}|value=") >=
                           1
                           && CountNew(after, [],
                               $"spot-state-command|rid={node.Rid}|spot={request.TargetSpotRid}|marker=sm-c3-send-{request.Marker}") >=
                           1
                           && CountNew(after, [],
                               $"spot-msg|rid={node.Rid}|spot={request.TargetSpotRid}|marker=sm-c3-publish-{request.Marker}") >=
                           1;
                },
                "Expected spot-to-spot request evidence.");
            return Results.Ok(result);
        });
        app.MapPost("/spot/to-spot/request-cross", async (
            IZLinkSpotClient routes,
            EvidenceStore evidence,
            NodeOptions node,
            SpotToSpotRouteReq request) =>
        {
            var before = evidence.Snapshot();
            var result = await RequestSpotToSpotAsync(
                routes,
                request.SourceSpotRid,
                new SpotToSpotReq(request.TargetSpotRid, request.Marker));
            await WaitUntilAsync(
                () => CountNew(evidence.Snapshot(), before,
                    $"spot-to-spot|rid={node.Rid}|source={request.SourceSpotRid}|target={request.TargetSpotRid}|value=") >= 1,
                "Expected source spot-to-spot evidence.");
            return Results.Ok(result);
        });
        app.MapPost("/spot/publish/wait", async (
            SpotPublishReq request,
            EvidenceStore evidence,
            NodeOptions node) =>
        {
            await WaitUntilAsync(
                () => CountNew(evidence.Snapshot(), Array.Empty<string>(),
                    $"spot-msg|rid={node.Rid}|spot={request.SpotRid}|marker={request.Marker}") >= 1,
                "SM-C4 expected publish event evidence.");
            return Results.Ok(new SpotPublishObserveRes(
                "spot.sm-c4-observe",
                request.SpotRid,
                request.Marker,
                true,
                evidence.Snapshot()));
        });
    }
}
