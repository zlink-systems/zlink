using System.Diagnostics;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Backend.Contracts;

namespace Zlink.Framework.UnitTests;

public sealed partial class StatefulServiceRuntimeTests
{
    public static IEnumerable<object[]> DurableSenderCases()
    {
        foreach (var operation in new[] { MeshOperationKind.ActorJoin,
                     MeshOperationKind.ActorCreate, MeshOperationKind.UserSpotCreate,
                     MeshOperationKind.UserSpotClose })
        foreach (var outcome in new[] { "absent", "withheld", "ready-after-submit" })
            yield return [(int)operation, outcome];
    }

    [Theory]
    [MemberData(nameof(DurableSenderCases))]
    public async Task DurableSenderPreservesExhaustionCauseAndOriginalOperation(
        int operationKind, string scenario)
    {
        var kind = (MeshOperationKind)operationKind;
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var source = NewNode(context, "matrix-source");
        var terminalCount = 0;
        await using var target = new ZLinkManagedMeshNode(context, "mesh",
            nativeTerminalReplySubmitOverride: reply =>
            {
                Interlocked.Increment(ref terminalCount);
                if (scenario != "withheld")
                    reply.Submit();
                return SubmitResult.Ok;
            });
        target.SetRoutingId(RoutingId.From("matrix-target"));
        var suffix = Guid.NewGuid().ToString("N");
        var targetEndpoint = $"inproc://matrix-target-{suffix}";
        source.SetBind($"inproc://matrix-source-{suffix}");
        target.SetBind(targetEndpoint);
        var spot = (ZLinkManagedSpot)target.GetOrCreateSpot("matrix-spot", out _);
        var createTarget = new RecordingActorCreateOperationTarget("mesh");
        var spotTarget = new RecordingUserSpotOperationTarget();
        target.SetActorCreateOperationTarget(createTarget);
        target.SetUserSpotOperationTarget(spotTarget);
        source.ObserveSpotAuthority(target.RoutingId, spot.SpotId,
            spot.LifecycleGeneration, target.Status().LifecycleGeneration,
            spot.AuthorityOwnerGeneration, 7);
        target.Start();
        source.Start();
        if (scenario == "withheld")
        {
            source.ConnectPeer(targetEndpoint, target.RoutingId);
            await WaitUntilAsync(() => source.Status().AdmittedPeerCount == 1
                                       && target.Status().AdmittedPeerCount == 1);
        }

        var timeout = TimeSpan.FromSeconds(2);
        var deadline = checked((ulong)DateTimeOffset.UtcNow.Add(timeout)
            .ToUnixTimeMilliseconds());
        var started = Stopwatch.GetTimestamp();
        var reservation = new ObjectReservationFence("matrix-reservation",
            "matrix-store", 11, 13, target.RoutingId,
            target.Status().LifecycleGeneration, "matrix-owner", 7, 1);
        var operation = source.AllocateOperationId();
        var submit = kind switch
        {
            MeshOperationKind.ActorJoin => source.TryRequestCanonicalActorJoin(
                new ZLinkBackendCanonicalActorJoinRequest(
                    new ZLinkBackendActorRef(source.RoutingId, "matrix-actor", 11),
                    source.Status().LifecycleGeneration, 13, 7, false,
                    target.RoutingId, spot.SpotId, spot.LifecycleGeneration,
                    target.Status().LifecycleGeneration, spot.AuthorityOwnerGeneration,
                    7, "ZLinkFrameworkActorJoinRequest", "application/json",
                    "{}"u8.ToArray()), operation, timeout),
            MeshOperationKind.ActorCreate => source.CreateActorRemote(target.RoutingId,
                "matrix-actor", "Sample.MatrixActor", reservation, deadline, operation,
                timeout),
            MeshOperationKind.UserSpotCreate => source.CreateUserSpot(target.RoutingId,
                "matrix-user-spot", "Sample.MatrixSpot", reservation, deadline,
                operation, timeout),
            MeshOperationKind.UserSpotClose => source.CloseUserSpot(target.RoutingId,
                new UserSpotCloseFence("matrix-user-spot", 11, target.RoutingId,
                    target.Status().LifecycleGeneration, 13, "matrix-store"),
                deadline, operation, timeout),
            _ => throw new ArgumentOutOfRangeException(nameof(kind))
        };
        Assert.Equal(SubmitResult.Ok, submit);
        if (scenario == "ready-after-submit")
        {
            // The durable operation already exists while no logical route can
            // submit. Connect the target inside that operation's same budget.
            await Task.Delay(50);
            source.ConnectPeer(targetEndpoint, target.RoutingId);
        }

        var joins = 0;
        if (kind == MeshOperationKind.ActorJoin && scenario != "absent")
        {
            await WaitUntilAsync(() =>
            {
                foreach (var record in DrainRecords(target))
                {
                    if (record.OperationKind != MeshOperationKind.ActorJoin)
                        continue;
                    // Command 28 carries the correlation; the source lifecycle
                    // remains a separate fence in that command's header.
                    Assert.Equal(new MeshOperationId(0, operation.Low), record.OperationId);
                    joins++;
                    Assert.Equal(SubmitResult.Ok,
                        record.ReplyJoin(ActorJoinResult.Accepted, Array.Empty<Message>()));
                }
                return joins != 0;
            });
        }
        var (completion, parts) = DrainCompletion(source, operation);
        ZLinkMessageParts.DisposeAll(parts);
        Assert.Equal(operation, completion.OperationId);
        Assert.Equal(kind, completion.OperationKind);
        var expected = scenario switch
        {
            "absent" => RequestResult.NotConnected,
            "withheld" => RequestResult.TimedOut,
            _ => RequestResult.Ok
        };
        Assert.Equal((int)expected, completion.TerminalResult);
        if (expected != RequestResult.Ok)
        {
            var error = Assert.IsType<ZLinkFrameworkException>(
                ZLinkRequestFailureMapper.CreateCompletionException(expected, "matrix"));
            Assert.Equal(scenario == "absent" ? ZLinkFrameworkErrorKind.Unavailable
                : ZLinkFrameworkErrorKind.DeadlineExceeded, error.Kind);
        }
        else
            Assert.True(Stopwatch.GetElapsedTime(started) < timeout);

        var executions = kind switch
        {
            MeshOperationKind.ActorJoin => joins,
            MeshOperationKind.ActorCreate => createTarget.CreateCount,
            MeshOperationKind.UserSpotCreate => spotTarget.CreateCount,
            _ => spotTarget.CloseCount
        };
        Assert.Equal(scenario == "absent" ? 0 : 1, executions);
        Assert.Equal(scenario == "absent" ? 0 : 1, Volatile.Read(ref terminalCount));
    }
}
