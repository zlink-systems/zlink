using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Backend.Contracts;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class ActorRelocationCompletionOrderingTests
{
    // 03-spot-actor/05-spot-actor-membership §4: Join completion, OperationId,
    // optional reply and retry cursor live only in the current source and
    // target process. No Store journal or Store-driven completion replay
    // remains in the runtime.
    [Fact]
    public void Join_completion_has_no_durable_journal_or_store_replay()
    {
        var assembly = typeof(ZLinkActorRuntimeState).Assembly;
        Assert.Null(
            assembly.GetType(
                "Zlink.Framework.Runtime.Actors.ZLinkDeferredActorJoinCompletionJournal"
            )
        );
        var runtimeMethods = typeof(Zlink.Framework.Runtime.Host.ZLinkFrameworkRuntime)
            .GetMethods(
                System.Reflection.BindingFlags.Instance
                    | System.Reflection.BindingFlags.Static
                    | System.Reflection.BindingFlags.Public
                    | System.Reflection.BindingFlags.NonPublic
            )
            .Select(static method => method.Name)
            .ToHashSet(StringComparer.Ordinal);
        Assert.DoesNotContain("RecoverCanonicalRemoteJoinCompletionAsync", runtimeMethods);
        Assert.DoesNotContain("RunDeferredJoinCompletionRecoveryAsync", runtimeMethods);
        Assert.DoesNotContain("RecoverPublishedRelocationsAsync", runtimeMethods);
    }

    [Fact]
    public async Task Target_completion_waits_for_the_actor_mailbox_turn()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        state.BindNativeActorRef(
            new ZLinkBackendActorRef(RoutingId.From("node-target"), "actor-1", 7)
        );
        state.BindActorInstance(new TestActor("actor-1"));
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var order = new List<string>();

        var current = state
            .ExecuteLifecycleAsync(
                async _ =>
                {
                    order.Add("message");
                    entered.SetResult();
                    await release.Task;
                },
                CancellationToken.None
            )
            .AsTask();
        await entered.Task;
        var completion = state
            .ExecuteRelocationCompletionAsync(
                7,
                _ =>
                {
                    order.Add("completion");
                    return ValueTask.CompletedTask;
                },
                CancellationToken.None
            )
            .AsTask();

        Assert.False(completion.IsCompleted);
        release.SetResult();
        await Task.WhenAll(current, completion);
        Assert.Equal(["message", "completion"], order);
    }
}
