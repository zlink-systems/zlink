using System.Diagnostics;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Backend.Contracts;

namespace Zlink.Framework.UnitTests;

public sealed partial class StatefulServiceRuntimeTests
{
    [Theory]
    [InlineData(true, false, false)]
    [InlineData(false, false, false)]
    [InlineData(true, true, false)]
    [InlineData(true, true, true)]
    public async Task DurableSenderIntentRemovalEndsUnavailableWhilePhysicalDisconnectReplays(
        bool removeIntent, bool removeExpectationFirst, bool ownerDisconnects)
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var source = NewNode(context, "lifecycle-source");
        var replySubmissions = 0;
        await using var target = new ZLinkManagedMeshNode(context, "mesh",
            nativeTerminalReplySubmitOverride: reply =>
            {
                if (Interlocked.Increment(ref replySubmissions) > 1)
                    reply.Submit();
                return SubmitResult.Ok;
            });
        target.SetRoutingId(RoutingId.From("lifecycle-target"));
        var suffix = Guid.NewGuid().ToString("N");
        var sourceEndpoint = $"inproc://lifecycle-source-{suffix}";
        var targetEndpoint = $"inproc://lifecycle-target-{suffix}";
        source.SetBind(sourceEndpoint);
        target.SetBind(targetEndpoint);
        var createTarget = new RecordingActorCreateOperationTarget("mesh");
        target.SetActorCreateOperationTarget(createTarget);
        source.SetPeerExpectation(target.RoutingId, targetEndpoint,
            ZLinkServiceSecurityIdentity.Plaintext, target.Status().LifecycleGeneration);
        source.Start();
        target.Start();
        // The source has an inbound peer, so closing the target's outbound
        // connection produces a physical disconnect without an automatic reconnect.
        if (ownerDisconnects)
            source.ConnectPeer(targetEndpoint, target.RoutingId);
        else
            target.ConnectPeer(sourceEndpoint, source.RoutingId);
        await WaitUntilAsync(() => source.Status().AdmittedPeerCount == 1
                                   && target.Status().AdmittedPeerCount == 1);
        var operation = source.AllocateOperationId();
        var timeout = TimeSpan.FromSeconds(5);
        var reservation = new ObjectReservationFence("lifecycle-reservation",
            "lifecycle-store", 11, 13, target.RoutingId,
            target.Status().LifecycleGeneration, "lifecycle-owner", 7, 1);
        Assert.Equal(SubmitResult.Ok, source.CreateActorRemote(target.RoutingId,
            "lifecycle-actor", "Sample.LifecycleActor", reservation,
            checked((ulong)DateTimeOffset.UtcNow.Add(timeout).ToUnixTimeMilliseconds()),
            operation, timeout));
        await WaitUntilAsync(() => Volatile.Read(ref replySubmissions) == 1);
        Assert.Equal(1, createTarget.CreateCount);
        if (removeExpectationFirst)
        {
            source.RemovePeerExpectation(target.RoutingId, targetEndpoint);
            Assert.DoesNotContain(DrainRecords(source), record => record.OperationId == operation);
        }
        var removedAt = Stopwatch.GetTimestamp();
        if (ownerDisconnects)
            source.DisconnectPeer(target.RoutingId);
        else
            target.DisconnectPeer(source.RoutingId);
        await WaitUntilAsync(() => source.Status().AdmittedPeerCount == 0);
        if (!ownerDisconnects)
        {
            await Task.Delay(100);
            Assert.DoesNotContain(DrainRecords(source), record => record.OperationId == operation);
        }

        if (removeIntent)
        {
            if (!ownerDisconnects)
            {
                removedAt = Stopwatch.GetTimestamp();
                // The owner also revisits removal when a previously admitted
                // peer disappears after its expectation was already removed.
                source.RemovePeerExpectation(target.RoutingId, targetEndpoint);
            }
            var (completion, parts) = DrainCompletion(source, operation);
            ZLinkMessageParts.DisposeAll(parts);
            Assert.Equal((int)RequestResult.NotConnected, completion.TerminalResult);
            Assert.True(Stopwatch.GetElapsedTime(removedAt) < TimeSpan.FromSeconds(1));
        }
        if (ownerDisconnects)
            source.ConnectPeer(targetEndpoint, target.RoutingId);
        else
            target.ConnectPeer(sourceEndpoint, source.RoutingId);
        await WaitUntilAsync(() => source.Status().AdmittedPeerCount == 1
                                   && target.Status().AdmittedPeerCount == 1);
        if (removeIntent)
        {
            await Task.Delay(100);
            Assert.Equal(1, Volatile.Read(ref replySubmissions));
            Assert.DoesNotContain(DrainRecords(source), record => record.OperationId == operation);
        }
        else
        {
            var (completion, parts) = DrainCompletion(source, operation);
            ZLinkMessageParts.DisposeAll(parts);
            Assert.Equal((int)RequestResult.Ok, completion.TerminalResult);
            Assert.Equal(2, Volatile.Read(ref replySubmissions));
        }
        Assert.Equal(1, createTarget.CreateCount);
    }

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
