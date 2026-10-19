using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests;

public sealed partial class StatefulServiceRuntimeTests
{
    [Fact]
    public async Task NativeSessionBind_IntentRemovalEndsWithoutWaitingForDeadline()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        var deadlineTime = new ControllableTimeProvider();
        await using var source = NewNode(
            context,
            "bind-intent-source",
            deadlineTimeProvider: deadlineTime
        );
        var target = RoutingId.From("bind-intent-target");
        var endpoint = $"inproc://bind-intent-{Guid.NewGuid():N}";
        source.SetPeerExpectation(target, endpoint, ZLinkServiceSecurityIdentity.Plaintext, 7);
        source.Start();
        var request = source
            .BindBoundSessionAsync(
                CanonicalBindRequest(target, 7),
                TimeSpan.FromSeconds(2),
                CancellationToken.None
            )
            .AsTask();
        Assert.False(request.IsCompleted);
        source.RemovePeerExpectation(target, endpoint);
        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            request.WaitAsync(TimeSpan.FromSeconds(1))
        );
        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, error.Kind);
    }

    [Fact]
    public async Task NativeDurablePending_ImmediateIntentRemovalPublishesTerminal()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        var deadlineTime = new ControllableTimeProvider();
        await using var source = NewNode(
            context,
            "bind-pending-source",
            deadlineTimeProvider: deadlineTime
        );
        var target = RoutingId.From("bind-pending-target");
        var endpoint = $"inproc://bind-pending-{Guid.NewGuid():N}";
        source.SetPeerExpectation(target, endpoint, ZLinkServiceSecurityIdentity.Plaintext, 7);
        source.Start();
        var reservation = new ObjectReservationFence(
            "bind-reservation",
            "bind-store",
            11,
            13,
            target,
            7,
            "bind-owner",
            17,
            1
        );
        // Intent retirement fixes the outcome immediately; the durable owner
        // publishes its record asynchronously. Keep the deadline clock frozen
        // and wait for that record's ready edge before consuming it.
        var completionReady = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        source.SetReadyHandler(domains =>
        {
            completionReady.TrySetResult();
            return domains;
        });
        var timeout = TimeSpan.FromSeconds(2);
        Assert.Equal(
            SubmitResult.Ok,
            source.CreateActorRemote(
                target,
                "pending-actor",
                "Sample.Actor",
                reservation,
                checked((ulong)deadlineTime.GetUtcNow().Add(timeout).ToUnixTimeMilliseconds()),
                out var operation,
                timeout
            )
        );
        source.RemovePeerExpectation(target, endpoint);
        await completionReady.Task.WaitAsync(timeout);
        var completion = Assert.Single(
            DrainRecords(source),
            record => record.OperationId == operation && record.Kind == MeshRecordKind.Completion
        );
        Assert.Equal((int)RequestResult.NotConnected, completion.TerminalResult);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task NativeSessionBind_WaitsForAdmissionAndConsumesOneTerminal(bool rejected)
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var source = NewNode(context, "bind-source");
        await using var target = NewNode(context, "bind-target");
        var suffix = Guid.NewGuid().ToString("N");
        source.SetBind($"inproc://bind-source-{suffix}");
        var endpoint = $"inproc://bind-target-{suffix}";
        target.SetBind(endpoint);
        source.Start();
        target.Start();
        var attempts = 0;
        target.SetBoundSessionBindHandler(
            (sourceRid, generation, binding, reply, cancellationToken) =>
            {
                attempts++;
                Assert.Equal(source.RoutingId, sourceRid);
                Assert.NotEqual(0UL, generation);
                Assert.Equal("bind-actor", binding.Actor.Actor.ActorId.Value);
                if (rejected)
                    return ValueTask.FromException(
                        new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.Unavailable,
                            "terminal rejection"
                        )
                    );
                reply();
                return ValueTask.CompletedTask;
            }
        );
        var request = source
            .BindBoundSessionAsync(
                CanonicalBindRequest(target.RoutingId, target.Status().LifecycleGeneration),
                TimeSpan.FromSeconds(2),
                CancellationToken.None
            )
            .AsTask();
        Assert.False(request.IsCompleted);
        source.ConnectPeer(endpoint, target.RoutingId);
        if (rejected)
        {
            var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() => request);
            Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, error.Kind);
        }
        else
        {
            var accepted = await request;
            Assert.NotNull(accepted);
            Assert.Equal(1UL, accepted.BindingGeneration.Value);
            Assert.Equal(3UL, accepted.AuthorityOwnerGeneration.Value);
        }
        Assert.Equal(1, attempts);
        using var ready = new MeshReadyBatch();
        target.DrainReady(MeshReadyDomains.Application, ready, RecvFlags.DontWait);
        Assert.Equal(0, ready.Count);
    }

    private static ServiceWireCodec.BoundSessionBind38 CanonicalBindRequest(
        RoutingId target,
        ulong generation
    ) =>
        new(
            0,
            new(1),
            new(
                new(new("bind-actor"), new(1)),
                new(target.ToBytes().ToArray()),
                new(generation),
                new(3),
                new(5)
            ),
            new(RoutingId.From("bind-session").ToBytes().ToArray()),
            new ServiceWireCodec.BoundSessionBindingTransitionCase0(
                ServiceWireCodec.BoundSessionBindingState.Active,
                new(1)
            )
        );
}
