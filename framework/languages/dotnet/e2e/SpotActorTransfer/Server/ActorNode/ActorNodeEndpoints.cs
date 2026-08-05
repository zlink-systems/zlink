using SpotActorTransfer.Shared;
using System.Collections.Concurrent;
using System.Diagnostics;
using Systems.Zlink;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace SpotActorTransfer.ActorNode;

internal static class ActorNodeEndpoints
{
    public static void Map(WebApplication app, ServerOptions options)
    {
        app.MapGet("/health", () => Results.Ok(new { status = "ok", options.Rid }));
        app.MapPost("/placement-weight", async (
            PlacementWeightReq request,
            IZLinkRouteMeshRuntimeOptions runtimeOptions,
            IZLinkRouteMeshRuntime meshRuntime,
            CancellationToken cancellationToken) =>
        {
            runtimeOptions.Mesh(SpotActorTransferNames.Mesh).PlacementWeight =
                request.Weight;
            var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(5);
            while (DateTimeOffset.UtcNow < deadline)
            {
                if (meshRuntime.GetStatus(SpotActorTransferNames.Mesh).IsReady
                    && runtimeOptions.Mesh(SpotActorTransferNames.Mesh)
                        .PlacementWeight == request.Weight)
                {
                    return Results.Ok(new PlacementWeightRes(request.Weight));
                }
                await Task.Delay(TimeSpan.FromMilliseconds(20), cancellationToken);
            }
            return Results.StatusCode(StatusCodes.Status503ServiceUnavailable);
        });
        app.MapGet("/mesh/ready", (
            IZLinkRouteMeshRuntime meshRuntime) =>
        {
            var status = meshRuntime.GetStatus(SpotActorTransferNames.Mesh);
            return Results.Ok(new MeshReadyRes(
                options.Rid,
                status.Peers
                    .Where(static peer => peer.State == ZLinkPeerState.Ready)
                    .Select(static peer => peer.NodeRid.ToString())
                    .ToArray(),
                [
                    SpotActorTransferNames.UserSpotType,
                    SpotActorTransferNames.RelocationPayloadUserSpotType,
                    SpotActorTransferNames.RelocationPayloadPerActorUserSpotType
                ]));
        });
        app.MapGet("/mesh/status", (
            IZLinkRouteMeshRuntime meshRuntime) =>
        {
            var status = meshRuntime.GetStatus(SpotActorTransferNames.Mesh);
            return Results.Ok(new
            {
                state = status.State.ToString(),
                status.IsReady,
                placementAvailable = status.Placement.IsAvailable,
                placementUnavailableReason = status.Placement.UnavailableReason?.ToString(),
                readyPeerCount = status.ReadyPeerCount,
                peers = status.Peers.Select(peer => new
                {
                    rid = peer.NodeRid.ToString(),
                    state = peer.State.ToString(),
                    reason = peer.UnavailableReason?.ToString()
                })
            });
        });
        app.MapGet("/evidence", (EvidenceStore evidence) => Results.Ok(evidence.Snapshot()));
        app.MapGet(
            "/relocation-blobs",
            (RelocationBlobObserver observer) =>
                Results.Ok(observer.Snapshot()));
        app.MapPost(
            "/relocation-blobs/reset",
            (RelocationBlobObserver observer) =>
            {
                observer.Reset();
                return Results.Ok();
            });
        app.MapGet("/process-memory", () =>
        {
            using var process = Process.GetCurrentProcess();
            return Results.Ok(new ProcessMemoryRes(
                process.WorkingSet64,
                process.PeakWorkingSet64));
        });
        app.MapPost("/payload-spots/user/{spotId}", async (
            string spotId,
            RelocationPayloadSpotReq request,
            IZLinkSpotManager spots,
            CancellationToken cancellationToken) =>
        {
            var created = await spots
                .GetOrCreate(
                    spotId,
                    SpotActorTransferNames.RelocationPayloadUserSpotType)
                .InMesh(SpotActorTransferNames.Mesh)
                .Request(request)
                .Async(cancellationToken);
            var state = TransferActorStateCodec.CreateState(
                spotId,
                request.ApplicationStateBytes);
            return Results.Ok(new RelocationPayloadSpotRes(
                created.Spot.SpotId,
                created.Spot.NodeRid.ToString(),
                checked((long)created.Spot.ObjectGeneration),
                state.Length,
                TransferActorStateCodec.Sha256(state)));
        });
        app.MapPost("/payload-spots/instance/{spotId}", async (
            string spotId,
            RelocationPayloadSpotReq request,
            IZLinkSpotClient spots,
            CancellationToken cancellationToken) =>
        {
            var result = await spots
                .RequestToSpot(spotId, request)
                .InstanceSpot(
                    SpotActorTransferNames.RelocationPayloadInstanceSpotType)
                .InMesh(SpotActorTransferNames.Mesh)
                .Timeout(TimeSpan.FromSeconds(30))
                .Async<RelocationPayloadSpotRes>(cancellationToken);
            return Results.Ok(result);
        });
        app.MapPost("/payload-spots/{spotId}/close", async (
            string spotId,
            IZLinkSpotManager spots,
            CancellationToken cancellationToken) =>
        {
            var spot = await spots.FindAsync(spotId, cancellationToken);
            return Results.Ok(spot is not null
                && await spots.CloseAsync(spot.Value, cancellationToken));
        });
        app.MapPost("/workload/actors/create-bulk", async (
            RelocationBulkActorCreateReq request,
            IZLinkActorManager actors,
            CancellationToken cancellationToken) =>
        {
            var actorIds = Enumerable.Range(0, request.Count)
                .Select(index =>
                    $"{request.ActorIdPrefix}-{index:D6}")
                .ToArray();
            var nodeRids = new string[actorIds.Length];
            await Parallel.ForEachAsync(
                Enumerable.Range(0, actorIds.Length),
                new ParallelOptions
                {
                    CancellationToken = cancellationToken,
                    MaxDegreeOfParallelism = Math.Clamp(
                        request.MaxConcurrency,
                        1,
                        256)
                },
                async (index, token) =>
                {
                    var actor = (await actors
                            .GetOrCreate(
                                actorIds[index],
                                request.ActorType)
                            .Request(new ActorCreateReq(
                                actorIds[index],
                                request.ActorType,
                                index,
                                request.ApplicationStateBytes))
                            .Async(token)) switch
                    {
                        ZLinkActorCreateResult.Existing value =>
                            value.Actor,
                        ZLinkActorCreateResult.Created value =>
                            value.Actor,
                        _ => throw new InvalidOperationException(
                            "Bulk Actor creation was rejected.")
                    };
                    nodeRids[index] = actor.NodeRid.ToString();
                });
            return Results.Ok(new RelocationBulkActorCreateRes(
                actorIds,
                nodeRids));
        });
        app.MapPost("/workload/spots/create-bulk", async (
            RelocationBulkSpotCreateReq request,
            IZLinkSpotManager spotManager,
            IZLinkSpotClient spotClient,
            IZLinkActorManager actorManager,
            IZLinkActorClient actorClient,
            CancellationToken cancellationToken) =>
        {
            var spotIds = Enumerable.Range(0, request.Count)
                .Select(index =>
                    $"{request.SpotIdPrefix}-{index:D6}")
                .ToArray();
            var nodeRids = new string[spotIds.Length];
            var spotObjectGenerations = new long[spotIds.Length];
            var actorIds = new ConcurrentBag<string>();
            await Parallel.ForEachAsync(
                Enumerable.Range(0, spotIds.Length),
                new ParallelOptions
                {
                    CancellationToken = cancellationToken,
                    MaxDegreeOfParallelism = Math.Clamp(
                        request.MaxConcurrency,
                        1,
                        256)
                },
                async (index, token) =>
                {
                    var payload = new RelocationPayloadSpotReq(
                        request.Scenario,
                        request.ApplicationStateBytes);
                    if (request.InstanceSpot)
                    {
                        var result = await spotClient
                            .RequestToSpot(spotIds[index], payload)
                            .InstanceSpot(
                                SpotActorTransferNames
                                    .RelocationPayloadInstanceSpotType)
                            .InMesh(SpotActorTransferNames.Mesh)
                            .Timeout(TimeSpan.FromSeconds(30))
                            .Async<RelocationPayloadSpotRes>(token);
                        nodeRids[index] = result.NodeRid;
                        spotObjectGenerations[index] =
                            result.ObjectGeneration;
                        return;
                    }

                    var created = await spotManager
                        .GetOrCreate(
                            spotIds[index],
                            request.SpotType
                            ?? (request.PerActor
                                ? SpotActorTransferNames
                                    .RelocationPayloadPerActorUserSpotType
                                : SpotActorTransferNames
                                    .RelocationPayloadUserSpotType))
                        .InMesh(SpotActorTransferNames.Mesh)
                        .Request(payload)
                        .Async(token);
                    nodeRids[index] =
                        created.Spot.NodeRid.ToString();
                    spotObjectGenerations[index] =
                        checked((long)created.Spot.ObjectGeneration);
                    // Membership commits for one Spot share one authority
                    // aggregate. Set up that aggregate in order; the outer
                    // Spot loop still creates independent Spots in parallel.
                    for (var actorIndex = 0;
                         actorIndex < request.ActorsPerSpot;
                         actorIndex++)
                    {
                        var actorId =
                            $"{request.SpotIdPrefix}-actor-"
                            + $"{index:D6}-{actorIndex:D6}";
                        _ = await actorManager
                            .GetOrCreate(
                                actorId,
                                SpotActorTransferNames
                                    .ActorTypeStateful)
                            .Request(new ActorCreateReq(
                                actorId,
                                SpotActorTransferNames
                                    .ActorTypeStateful,
                                actorIndex,
                                request.ActorApplicationStateBytes))
                            .Async(token);
                        var joined = await actorClient
                            .RequestToActor(
                                actorId,
                                new JoinTargetReq(
                                    request.Scenario,
                                    spotIds[index]))
                            .Timeout(TimeSpan.FromSeconds(30))
                            .Async<JoinTargetRes>(token);
                        if (!joined.Accepted)
                            throw new InvalidOperationException(
                                $"Bulk Spot member Actor '{actorId}' "
                                + "was rejected.");
                        actorIds.Add(actorId);
                    }
                });
            return Results.Ok(new RelocationBulkSpotCreateRes(
                spotIds,
                nodeRids,
                spotObjectGenerations,
                actorIds.Order(StringComparer.Ordinal).ToArray()));
        });
        app.MapGet("/workload/message-flow", (
            RelocationMessageFlowEvidenceStore flows) =>
            Results.Ok(flows.Snapshot()));
        app.MapPost("/workload/locations", async (
            RelocationLocationQueryReq request,
            IZLinkActorManager actors,
            IZLinkSpotManager spots,
            CancellationToken cancellationToken) =>
        {
            var result =
                new ConcurrentBag<RelocationLocationSnapshot>();
            await Parallel.ForEachAsync(
                request.ActorIds,
                new ParallelOptions
                {
                    CancellationToken = cancellationToken,
                    MaxDegreeOfParallelism = 64
                },
                async (actorId, token) =>
                {
                    var actor =
                        await actors.FindAsync(actorId, token)
                        ?? throw new InvalidOperationException(
                            $"Actor '{actorId}' location is missing.");
                    result.Add(new RelocationLocationSnapshot(
                        "actor",
                        actor.ActorId,
                        checked((long)actor.ObjectGeneration),
                        actor.NodeRid.ToString()));
                });
            await Parallel.ForEachAsync(
                request.SpotIds,
                new ParallelOptions
                {
                    CancellationToken = cancellationToken,
                    MaxDegreeOfParallelism = 64
                },
                async (spotId, token) =>
                {
                    var spot =
                        await spots.FindAsync(spotId, token)
                        ?? throw new InvalidOperationException(
                            $"Spot '{spotId}' location is missing.");
                    result.Add(new RelocationLocationSnapshot(
                        "spot",
                        spot.SpotId,
                        checked((long)spot.ObjectGeneration),
                        spot.NodeRid.ToString()));
                });
            return Results.Ok(result
                .OrderBy(static item => item.ObjectKind)
                .ThenBy(static item => item.ObjectId)
                .ToArray());
        });
        app.MapPost("/relocate", async (
            RelocateHostReq request,
            IZLinkFrameworkRuntime runtime,
            CancellationToken cancellationToken) =>
        {
            var mode = request.TargetApplicationVersion is null
                ? ZLinkFrameworkRelocationMode.PlannedMaintenance
                : ZLinkFrameworkRelocationMode.RollingUpdate;
            var result = await runtime.RelocateAsync(
                new ZLinkFrameworkRelocationOptions
                {
                    Mode = mode,
                    TargetApplicationVersion =
                        request.TargetApplicationVersion,
                    Deadline = TimeSpan.FromMilliseconds(
                        request.DeadlineMilliseconds)
                },
                cancellationToken);
            return Results.Ok(new RelocateHostRes(
                result.Outcome.ToString(),
                result.Reason.ToString(),
                runtime.Status.State.ToString()));
        });
        app.MapPost("/evidence/wait", async (EvidenceWaitReq request, EvidenceStore evidence,
            CancellationToken cancellationToken) =>
        {
            var snapshot = await evidence.WaitUntilAsync(
                entries => request.ContainsAll.All(expected => entries.Any(entry =>
                    EvidenceText(entry).Contains(expected, StringComparison.Ordinal))),
                TimeSpan.FromMilliseconds(Math.Clamp(request.TimeoutMilliseconds, 1, 30000)),
                cancellationToken);
            return Results.Ok(snapshot);
        });
        app.MapPost("/runtime-evidence/wait", async (EvidenceWaitReq request,
            RuntimeEvidenceStore evidence, CancellationToken cancellationToken) =>
        {
            var snapshot = await evidence.WaitUntilAsync(
                request.ContainsAll,
                TimeSpan.FromMilliseconds(Math.Clamp(request.TimeoutMilliseconds, 1, 30000)),
                cancellationToken);
            return Results.Ok(snapshot);
        });
        app.MapPost("/relocation-interruption/wait", async (
            RelocationInterruptionWaitReq request,
            RelocationInterruptionEvidenceStore evidence,
            CancellationToken cancellationToken) =>
        {
            var snapshot = await evidence.WaitUntilAsync(
                request.UnitKind,
                request.ExecutionMode,
                Math.Clamp(request.MinimumCount, 1, 10_000),
                TimeSpan.FromMilliseconds(Math.Clamp(
                    request.TimeoutMilliseconds,
                    1,
                    30_000)),
                cancellationToken);
            return Results.Ok(snapshot);
        });
        app.MapPost("/joined-gates/{spotId}/release", (string spotId, JoinedGateStore gates) =>
            Results.Ok(new GateReleaseRes(spotId, gates.Release(spotId))));
        app.MapPost("/transfer-gates/{actorId}/release", (string actorId, TransferGateStore gates) =>
            Results.Ok(new GateReleaseRes(actorId, gates.Release(actorId))));
        app.MapPost("/cleanup-gates/{actorId}/arm", (
            string actorId, CleanupGateArmReq request, ActorCleanupGateStore gates) =>
            Results.Ok(new CleanupGateRes(actorId, gates.Arm(actorId, request.Scenario))));
        app.MapPost("/cleanup-gates/{actorId}/allow-attempt", (
            string actorId, ActorCleanupGateStore gates) =>
            Results.Ok(new CleanupGateRes(actorId, gates.AllowAttempt(actorId))));
        app.MapPost("/cleanup-gates/{actorId}/release", (string actorId, ActorCleanupGateStore gates) =>
            Results.Ok(new CleanupGateRes(actorId, gates.Release(actorId))));
        app.MapPost("/spots", async (CreateSpotReq request, IZLinkSpotManager spots,
            CancellationToken cancellationToken) =>
        {
            var result = await spots
                .GetOrCreate(
                    request.SpotId,
                    SpotActorTransferNames.UserSpotType)
                .InMesh(SpotActorTransferNames.Mesh)
                .Request(request)
                .Async(cancellationToken);
            return Results.Ok(new CreateSpotRes(
                result.Spot.SpotId, result.Spot.NodeRid.ToString(), result.State.ToString()));
        });
        app.MapPost("/actors", async (ActorCreateReq request, IZLinkActorManager actors,
            CancellationToken cancellationToken) =>
        {
            var actor = (await actors.GetOrCreate(request.ActorId, request.ActorType)
                .Request(request).Async(cancellationToken)) switch
            {
                ZLinkActorCreateResult.Existing value => value.Actor,
                ZLinkActorCreateResult.Created value => value.Actor,
                _ => throw new InvalidOperationException("Actor creation was rejected.")
            };
            return Results.Ok(new ActorCreateRes(
                actor.ActorId, request.ActorType, actor.NodeRid.ToString(), checked((long)actor.ObjectGeneration)));
        });
        app.MapGet("/actors/{actorId}/ref", async (string actorId, IZLinkActorManager actors,
            CancellationToken cancellationToken) =>
        {
            var actor = await FindActorAsync(actors, actorId, cancellationToken);
            return Results.Ok(new ActorRefRes(
                actor.ActorId, actor.NodeRid.ToString(), checked((long)actor.ObjectGeneration)));
        });
        app.MapPost("/actors/{actorId}/destroy", async (
            string actorId,
            IZLinkActorManager actors,
            CancellationToken cancellationToken) =>
        {
            var actor = await FindActorAsync(actors, actorId, cancellationToken);
            var destroyed = await actors.DestroyAsync(actor, cancellationToken);
            return Results.Ok(new ActorDestroyRes(
                actor.ActorId,
                checked((long)actor.ObjectGeneration),
                destroyed));
        });
        app.MapGet("/actors/{actorId}/ref-evidence/{scenario}/{marker}", async (
            string actorId,
            string scenario,
            string marker,
            IZLinkActorManager actors,
            EvidenceStore evidence,
            CancellationToken cancellationToken) =>
        {
            var actor = await FindActorAsync(actors, actorId, cancellationToken);
            var snapshot = new ActorRefRes(
                actor.ActorId, actor.NodeRid.ToString(), checked((long)actor.ObjectGeneration));
            evidence.Add(scenario, actorId, marker,
                $"node={snapshot.NodeRid};generation={snapshot.Generation}");
            return Results.Ok(snapshot);
        });
        app.MapPost("/actors/{actorId}/join", async (string actorId, JoinTargetReq request,
            IZLinkActorManager actors,
            IZLinkActorClient actorClient,
            EvidenceStore evidence,
            CancellationToken cancellationToken) =>
        {
            var actor = await FindActorAsync(actors, actorId, cancellationToken);
            try
            {
                var reply = await actorClient.RequestToActor(actor.ActorId, request)
                    .Timeout(TimeSpan.FromSeconds(10)).Async<JoinTargetRes>(cancellationToken);
                return Results.Ok(reply);
            }
            catch (Exception error) when (error is ZLinkFrameworkException or InvalidOperationException)
            {
                var kind = error is ZLinkFrameworkException frameworkError
                    ? frameworkError.Kind.ToString()
                    : error.Message;
                //  The evidence line carries only the kind because scenarios
                //  match on it. Losing the message and stack too left a failure
                //  with no way to tell which of the many throw sites produced it.
                Console.Error.WriteLine(
                    $"[join-failed] actor={actorId} scenario={request.Scenario} {error}");
                evidence.Add(request.Scenario, actorId, "join_failed", kind);
                return Results.Ok(new { request.Scenario, ActorId = actorId, Accepted = false, ErrorKind = kind });
            }
        });
        app.MapPost("/actors/{actorId}/probe", async (string actorId, ProbeReq request,
            IZLinkActorManager actors, IZLinkActorClient actorClient, EvidenceStore evidence,
            CancellationToken cancellationToken) =>
        {
            var actor = await FindActorAsync(actors, actorId, cancellationToken);
            evidence.Add(request.Scenario, actorId, "probe_submitted", request.Marker);
            return Results.Ok(await actorClient.RequestToActor(actor.ActorId, request)
                .Timeout(TimeSpan.FromSeconds(10)).Async<ProbeRes>(cancellationToken));
        });
        app.MapPost("/actors/{actorId}/probe-from-node", async (
            string actorId,
            NodeActorCallReq request,
            IZLinkActorClient actorClient, CancellationToken cancellationToken) =>
        {
            // The HTTP endpoint selects the process that submits the call.
            // Framework routing still uses only the public global Actor ID.
            try
            {
                var response = await actorClient.RequestToActor(
                        actorId,
                        new ProbeReq(
                            request.Scenario,
                            request.Marker,
                            request.ReplyMarker))
                    .Timeout(TimeSpan.FromMilliseconds(request.TimeoutMs))
                    .Async<ProbeRes>(cancellationToken);
                return Results.Ok(new NodeActorProbeRes(true, response, null));
            }
            catch (ZLinkFrameworkException error)
            {
                return Results.Ok(new NodeActorProbeRes(false, null, error.Kind.ToString()));
            }
            catch (Exception error) when (error is InvalidOperationException or TimeoutException)
            {
                return Results.Ok(new NodeActorProbeRes(false, null, error.GetType().Name));
            }
        });
        app.MapPost("/actors/{actorId}/send-from-node", async (
            string actorId,
            NodeActorCallReq request,
            IZLinkActorClient actorClient, CancellationToken cancellationToken) =>
        {
            // The selected process may hold a stale bounded route. The
            // application does not supply an owner RID or ObjectGeneration.
            await actorClient.SendToActor(
                    actorId,
                    new HandoffPacket(request.Scenario, request.Marker))
                .Async(cancellationToken);
            return Results.Ok();
        });
        app.MapPost("/workload/actors/request", async (
            RelocationWorkloadCallReq request,
            IZLinkActorClient actors,
            CancellationToken cancellationToken) =>
        {
            var reply = await actors
                .RequestToActor(
                    request.TargetId,
                    new RelocationWorkloadRequest(
                        request.Scenario,
                        request.Sequence,
                        request.OperationId,
                        request.SentUnixTimeMilliseconds,
                        request.AbsoluteDeadlineUnixTimeMilliseconds))
                .Metadata(
                    RelocationWorkloadMetadata.OperationId,
                    request.OperationId)
                .Timeout(RemainingTimeout(request))
                .Async<RelocationWorkloadReply>(cancellationToken);
            return Results.Ok(reply);
        });
        app.MapPost("/workload/actors/request-probe", async (
            RelocationWorkloadCallReq request,
            IZLinkActorClient actors,
            CancellationToken cancellationToken) =>
        {
            try
            {
                var reply = await actors
                    .RequestToActor(
                        request.TargetId,
                        new RelocationWorkloadRequest(
                            request.Scenario,
                            request.Sequence,
                            request.OperationId,
                            request.SentUnixTimeMilliseconds,
                            request.AbsoluteDeadlineUnixTimeMilliseconds))
                    .Metadata(
                        RelocationWorkloadMetadata.OperationId,
                        request.OperationId)
                    .Timeout(RemainingTimeout(request))
                    .Async<RelocationWorkloadReply>(cancellationToken);
                return Results.Ok(new RelocationWorkloadProbeRes(true, reply, null));
            }
            catch (ZLinkFrameworkException error)
            {
                return Results.Ok(new RelocationWorkloadProbeRes(
                    false,
                    null,
                    error.Kind.ToString()));
            }
            catch (TimeoutException)
            {
                return Results.Ok(new RelocationWorkloadProbeRes(
                    false,
                    null,
                    nameof(ZLinkFrameworkErrorKind.DeadlineExceeded)));
            }
        });
        app.MapPost("/workload/actors/send", async (
            RelocationWorkloadCallReq request,
            IZLinkActorClient actors,
            CancellationToken cancellationToken) =>
        {
            await actors
                .SendToActor(
                    request.TargetId,
                    new RelocationWorkloadPacket(
                        request.Scenario,
                        request.Sequence,
                        request.OperationId,
                        request.SentUnixTimeMilliseconds,
                        request.AbsoluteDeadlineUnixTimeMilliseconds))
                .Metadata(
                    RelocationWorkloadMetadata.OperationId,
                    request.OperationId)
                .Async(cancellationToken);
            return Results.Ok();
        });
        app.MapPost("/workload/actors/block", async (
            RelocationQueueBlockReq request,
            IZLinkActorClient actors,
            CancellationToken cancellationToken) =>
        {
            var reply = await actors
                .RequestToActor(request.TargetId, request)
                .Timeout(TimeSpan.FromSeconds(30))
                .Async<RelocationQueueBlockRes>(cancellationToken);
            return Results.Ok(reply);
        });
        app.MapPost("/workload/spots/request", async (
            RelocationWorkloadCallReq request,
            IZLinkSpotClient spots,
            CancellationToken cancellationToken) =>
        {
            var reply = await spots
                .RequestToSpot(
                    request.TargetId,
                    new RelocationWorkloadRequest(
                        request.Scenario,
                        request.Sequence,
                        request.OperationId,
                        request.SentUnixTimeMilliseconds,
                        request.AbsoluteDeadlineUnixTimeMilliseconds))
                .Metadata(
                    RelocationWorkloadMetadata.OperationId,
                    request.OperationId)
                .Timeout(RemainingTimeout(request))
                .Async<RelocationWorkloadReply>(cancellationToken);
            return Results.Ok(reply);
        });
        app.MapPost("/workload/spots/request-probe", async (
            RelocationWorkloadCallReq request,
            IZLinkSpotClient spots,
            CancellationToken cancellationToken) =>
        {
            try
            {
                var reply = await spots
                    .RequestToSpot(
                        request.TargetId,
                        new RelocationWorkloadRequest(
                            request.Scenario,
                            request.Sequence,
                            request.OperationId,
                            request.SentUnixTimeMilliseconds,
                            request.AbsoluteDeadlineUnixTimeMilliseconds))
                    .Metadata(
                        RelocationWorkloadMetadata.OperationId,
                        request.OperationId)
                    .Timeout(RemainingTimeout(request))
                    .Async<RelocationWorkloadReply>(cancellationToken);
                return Results.Ok(new RelocationWorkloadProbeRes(true, reply, null));
            }
            catch (ZLinkFrameworkException error)
            {
                return Results.Ok(new RelocationWorkloadProbeRes(
                    false,
                    null,
                    error.Kind.ToString()));
            }
            catch (TimeoutException)
            {
                return Results.Ok(new RelocationWorkloadProbeRes(
                    false,
                    null,
                    nameof(ZLinkFrameworkErrorKind.DeadlineExceeded)));
            }
        });
        app.MapPost("/workload/spots/send", async (
            RelocationWorkloadCallReq request,
            IZLinkSpotClient spots,
            CancellationToken cancellationToken) =>
        {
            await spots
                .SendToSpot(
                    request.TargetId,
                    new RelocationWorkloadPacket(
                        request.Scenario,
                        request.Sequence,
                        request.OperationId,
                        request.SentUnixTimeMilliseconds,
                        request.AbsoluteDeadlineUnixTimeMilliseconds))
                .Metadata(
                    RelocationWorkloadMetadata.OperationId,
                    request.OperationId)
                .Async(cancellationToken);
            return Results.Ok();
        });
        app.MapPost("/workload/spots/block", async (
            RelocationQueueBlockReq request,
            IZLinkSpotClient spots,
            CancellationToken cancellationToken) =>
        {
            var reply = await spots
                .RequestToSpot(request.TargetId, request)
                .Timeout(TimeSpan.FromSeconds(30))
                .Async<RelocationQueueBlockRes>(cancellationToken);
            return Results.Ok(reply);
        });
        app.MapPost("/workload/spots/relocation-ready", async (
            RelocationWorkloadReadyCallReq request,
            IZLinkSpotClient spots,
            CancellationToken cancellationToken) =>
        {
            var reply = await spots
                .RequestToSpot(
                    request.SpotId,
                    new RelocationReadySignalReq(
                        request.Scenario,
                        request.DeferTwice,
                        request.StartFrameworkOperationAfterDefer))
                .Timeout(TimeSpan.FromSeconds(10))
                .Async<RelocationReadySignalRes>(cancellationToken);
            return Results.Ok(reply);
        });
        app.MapPost("/actors/{actorId}/bound-push", async (string actorId, BoundPushReq request,
            IZLinkActorManager actors, IZLinkActorClient actorClient, CancellationToken cancellationToken) =>
        {
            var actor = await FindActorAsync(actors, actorId, cancellationToken);
            return Results.Ok(await actorClient.RequestToActor(actor.ActorId, request)
                .Timeout(TimeSpan.FromSeconds(10)).Async<BoundPushRes>(cancellationToken));
        });
        app.MapPost("/shutdown", (IHostApplicationLifetime lifetime) =>
        {
            lifetime.StopApplication();
            return Results.Ok(new { status = "stopping" });
        });
        app.MapPost("/drain", async (
            IZLinkFrameworkRuntime runtime,
            CancellationToken cancellationToken) =>
        {
            var result = await runtime.ShutdownAsync(TimeSpan.FromSeconds(10), cancellationToken);
            if (result.Outcome != ZLinkFrameworkTerminationOutcome.Stopped)
                throw new InvalidOperationException($"Target drain did not complete: {result}.");
            return Results.Ok(new { status = "drained" });
        });
    }

    private static async ValueTask<ActorRef> FindActorAsync(
        IZLinkActorManager actors, string actorId, CancellationToken cancellationToken) =>
        await actors.FindAsync(actorId, cancellationToken)
        ?? throw new InvalidOperationException($"Actor '{actorId}' was not found.");

    private static string EvidenceText(ActorEvidence evidence) =>
        $"{evidence.Scenario}|{evidence.ActorId}|{evidence.Kind}|{evidence.Value}|{evidence.NodeRid}";

    private static TimeSpan RemainingTimeout(
        RelocationWorkloadCallReq request)
    {
        var remaining = TimeSpan.FromMilliseconds(
            request.AbsoluteDeadlineUnixTimeMilliseconds
            - DateTimeOffset.UtcNow.ToUnixTimeMilliseconds());
        if (remaining <= TimeSpan.Zero)
            throw new TimeoutException(
                $"Workload operation '{request.OperationId}' deadline elapsed.");
        return remaining < TimeSpan.FromMilliseconds(
            request.TimeoutMilliseconds)
            ? remaining
            : TimeSpan.FromMilliseconds(request.TimeoutMilliseconds);
    }
}
