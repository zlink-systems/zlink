using System.Diagnostics;
using System.Runtime.CompilerServices;
using Microsoft.Extensions.DependencyInjection;
using Xunit.Abstractions;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests.Runtime;

// #1674 request-serial: per-request costs measured against same-runtime controls.
public sealed class RequestSerialCostTests(ITestOutputHelper output)
{
    private const int Iterations = 1000;

    // Spec 08 §4.3 W1-W3: inbound admission is one runtime lane turn and the
    // drain seal read it makes. Both decisions run in their owners' turns
    // without a per-request closure or delegate.
    [Fact]
    public async Task Inbound_admission_allocates_only_its_runtime_lane_turn()
    {
        var services = new ServiceCollection();
        services.AddZLinkFramework(_ => { });
        await using var provider = services.BuildServiceProvider();
        var runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();
        _ = await runtime.EnsureStartedStateAsync(CancellationToken.None);
        await using var runtimeLane = new ZLinkStateLane();
        await using var gateLane = new ZLinkStateLane();
        var lane = (runtimeLane, gateLane);
        try
        {
            for (var index = 0; index < 128; index++)
            {
                AdmitAndExit(runtime);
                Control(lane);
            }
            var accepted = 0;
            for (var sample = 0; sample < 12; sample++)
            {
                long actual = 0;
                long baseline = 0;
                var collection = Collections();
                for (var index = 0; index < Iterations; index++)
                {
                    var before = GC.GetAllocatedBytesForCurrentThread();
                    var admission = runtime
                        .TryAdmitInboundOperationAsync(countAsRequest: true, ownsObjectWork: false)
                        .GetAwaiter()
                        .GetResult();
                    actual += GC.GetAllocatedBytesForCurrentThread() - before;
                    var entered = admission.EnterInbound();
                    Assert.True(entered.Accepted);
                    entered.Lease.Dispose();
                    before = GC.GetAllocatedBytesForCurrentThread();
                    Control(lane);
                    baseline += GC.GetAllocatedBytesForCurrentThread() - before;
                }
                if (collection != Collections())
                    continue;
                output.WriteLine(
                    $"inbound admission {actual / Iterations} B/op; runtime and drain-seal lane turn control {baseline / Iterations} B/op"
                );
                // The control holds the same two nested owner turns; the
                // execution-context maps they build depend on the caller's
                // ambient values, so the admission is bounded by it.
                Assert.True(
                    actual <= baseline,
                    $"inbound admission {actual}B, nested owner turns {baseline}B per {Iterations}"
                );
                accepted++;
            }
            Assert.True(accepted > 0);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }

        static void AdmitAndExit(ZLinkFrameworkRuntime runtime)
        {
            var entered = runtime
                .TryAdmitInboundOperationAsync(countAsRequest: true, ownsObjectWork: false)
                .GetAwaiter()
                .GetResult()
                .EnterInbound();
            entered.Lease.Dispose();
        }

        // The drain seal read runs inside the runtime turn of an asynchronous
        // admission method, as in the runtime.
        [MethodImpl(MethodImplOptions.NoInlining)]
        static void Control((ZLinkStateLane Runtime, ZLinkStateLane Gate) lanes) =>
            GC.KeepAlive(ControlAsync(lanes).GetAwaiter().GetResult());

        static async ValueTask<bool> ControlAsync(
            (ZLinkStateLane Runtime, ZLinkStateLane Gate) lanes
        )
        {
            var admission = await lanes
                .Runtime.RunAsync(
                    static gate =>
                        (
                            Sealed: gate.RunAsync(static value => value, false)
                                .GetAwaiter()
                                .GetResult(),
                            Accepted: true,
                            Neutral: false
                        ),
                    lanes.Gate
                )
                .ConfigureAwait(false);
            return admission.Accepted;
        }
    }

    // Spec 08 §4.3 W1 / §4.1 I4: a worker claims an owner's FIFO prefix in one
    // mailbox turn and returns the claim with one posted turn. Neither turn
    // creates a per-claim closure or delegate.
    [Fact]
    public async Task Owned_mailbox_claim_allocates_only_its_lane_turns()
    {
        var ready = 0;
        var mailbox = new ZLinkMeshNodeOwnedMailbox(
            static _ => { },
            static _ => { },
            () => ready++,
            static _ => { }
        );
        await using var lane = new ZLinkStateLane();
        using var batch = new MeshReceiveBatch();
        try
        {
            for (var index = 0; index < 128; index++)
            {
                Claim();
                Control(lane);
            }
            var accepted = 0;
            for (var sample = 0; sample < 12; sample++)
            {
                long actual = 0;
                long baseline = 0;
                var collection = Collections();
                for (var index = 0; index < Iterations; index++)
                {
                    actual += Claim();
                    var before = GC.GetAllocatedBytesForCurrentThread();
                    Control(lane);
                    baseline += GC.GetAllocatedBytesForCurrentThread() - before;
                }
                if (collection != Collections())
                    continue;
                output.WriteLine(
                    $"owned mailbox claim {actual / Iterations} B/op; lane turn + post control {baseline / Iterations} B/op"
                );
                // Same turn kinds as the control; the execution-context maps
                // they build depend on the caller's ambient values.
                Assert.True(
                    actual <= baseline,
                    $"owned mailbox claim {actual}B, lane turn + post {baseline}B per {Iterations}"
                );
                accepted++;
            }
            Assert.True(accepted > 0);
            Assert.True(ready > 0);
        }
        finally
        {
            mailbox.Dispose();
        }

        long Claim()
        {
            Assert.True(
                mailbox.TryEnqueue(new ZLinkMeshQueuedRecord(default, Array.Empty<Message>(), 0))
            );
            var before = GC.GetAllocatedBytesForCurrentThread();
            var claimed = mailbox.ClaimAndDrainAsync(batch, 1).GetAwaiter().GetResult();
            mailbox.Release();
            var allocated = GC.GetAllocatedBytesForCurrentThread() - before;
            Assert.True(claimed);
            batch.Reset();
            return allocated;
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        static void Control(ZLinkStateLane lane)
        {
            GC.KeepAlive(lane.RunAsync(static value => value, false).GetAwaiter().GetResult());
            lane.TryPost(static _ => { }, lane);
        }
    }

    private static (int Gen0, int Gen1, int Gen2) Collections() =>
        (GC.CollectionCount(0), GC.CollectionCount(1), GC.CollectionCount(2));
}
