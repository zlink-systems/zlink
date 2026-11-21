using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Xunit.Abstractions;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Internal.Locations;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class InboundOperationAdmissionTests(ITestOutputHelper output)
{
#if DEBUG
    [Theory]
    [InlineData(false, false, false)]
    [InlineData(false, true, false)]
    [InlineData(true, false, false)]
    [InlineData(true, true, false)]
    [InlineData(false, false, true)]
    [InlineData(false, true, true)]
    [InlineData(true, false, true)]
    [InlineData(true, true, true)]
    public async Task BusyRuntimeLane_AdmissionCompletesInCallerTurn(
        bool countAsRequest,
        bool ownsObjectWork,
        bool nested
    )
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Logging.ClearProviders();
        builder.Services.AddZLinkFramework(_ => { });
        using var host = builder.Build();
        var runtime = host.Services.GetRequiredService<ZLinkFrameworkRuntime>();
        await runtime.StartAsync(CancellationToken.None);
        ZLinkStateLane? lane = null;
        ZLinkStateLane.WorkObserver.Value = (_, current) => lane = current;
        runtime.SnapshotOperationAdmissions();
        ZLinkStateLane.WorkObserver.Value = null;
        Assert.NotNull(lane);
        var outer = nested
            ? runtime.TryEnterInboundOperation(countAsRequest: false, ownsObjectWork: false).Lease
            : ZLinkFrameworkRuntime.ZLinkRuntimeOperationLease.None;
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var held = lane.RunAsync(async () =>
        {
            entered.SetResult();
            await release.Task;
        });
        await entered.Task;
        var admission = runtime.TryAdmitInboundOperationAsync(countAsRequest, ownsObjectWork);
        var completedInClaimTurn = admission.IsCompletedSuccessfully;
        output.WriteLine(
            $"caller={Environment.CurrentManagedThreadId}, admission completed in claim turn={completedInClaimTurn}"
        );
        release.SetResult();
        await held;
        try
        {
            using ((await admission).Enter())
                Assert.True(
                    completedInClaimTurn,
                    "Inbound admission left the caller turn while the runtime state lane was occupied."
                );
        }
        finally
        {
            outer.Dispose();
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task RelocationFence_ClosedOwnerInboundDoesNotInvalidateBaseline(
        bool countAsRequest
    )
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Logging.ClearProviders();
        builder.Services.AddZLinkFramework(_ => { });
        // An owner without a claimed lease has no bound execution admission.
        builder.Services.AddSingleton(
            new ZLinkLocationRuntime(new ZLinkLocationOptions(), new ZLinkInMemoryLocationStore())
        );
        using var host = builder.Build();
        var runtime = host.Services.GetRequiredService<ZLinkFrameworkRuntime>();
        await runtime.StartAsync(CancellationToken.None);
        try
        {
            Assert.False(
                host.Services.GetRequiredService<ZLinkLocationRuntime>().IsBoundExecutionOpen
            );
            ZLinkStateLane? lane = null;
            ZLinkStateLane.WorkObserver.Value = (_, current) => lane = current;
            var baseline = runtime.SnapshotOperationAdmissions();
            ZLinkStateLane.WorkObserver.Value = null;
            Assert.NotNull(lane);
            var actorBaseline = runtime.DrainAdmission.SnapshotActorAdmissions();
            var refused = 0;
            // Inject a batch after the drain gate prepares the fence, before its runtime
            // baseline comparison. No timing or background traffic rate is involved.
            ZLinkStateLane.WorkObserver.Value = (_, current) =>
            {
                if (!ReferenceEquals(current, lane))
                    return;
                ZLinkStateLane.WorkObserver.Value = null;
                for (var index = 0; index < 32; index++)
                {
                    var inbound = runtime.TryEnterInboundOperation(countAsRequest);
                    using (inbound.Lease)
                        Assert.False(inbound.Accepted);
                    refused++;
                }
            };
            var fence = await runtime.TryBeginRelocationAdmissionFenceAsync(
                baseline,
                actorBaseline,
                new ZLinkActorHandoffDrainSnapshot(0, true),
                CancellationToken.None
            );
            ZLinkStateLane.WorkObserver.Value = null;
            var after = runtime.SnapshotOperationAdmissions();
            output.WriteLine(
                $"refused={refused}, baseline={baseline}, after={after}, fence committed={fence is not null}"
            );
            Assert.Equal(32, refused);
            Assert.NotNull(fence);
            Assert.Equal(baseline, after);
        }
        finally
        {
            ZLinkStateLane.WorkObserver.Value = null;
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task BusyRuntimeLane_RetainedOperationKeepsDrainPendingAfterParentReleases()
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Logging.ClearProviders();
        builder.Services.AddZLinkFramework(_ => { });
        using var host = builder.Build();
        var runtime = host.Services.GetRequiredService<ZLinkFrameworkRuntime>();
        await runtime.StartAsync(CancellationToken.None);
        ZLinkStateLane? lane = null;
        ZLinkStateLane.WorkObserver.Value = (_, current) => lane = current;
        runtime.SnapshotOperationAdmissions();
        ZLinkStateLane.WorkObserver.Value = null;
        Assert.NotNull(lane);
        var parent = runtime.TryEnterInboundOperation(countAsRequest: true).Lease;
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var held = lane.RunAsync(async () => await release.Task);
        ZLinkFrameworkRuntime.ZLinkRuntimeOperationLease? retained = null;
        var retaining = Task.Run(runtime.RetainOperationForBackgroundWork);
        try
        {
            try
            {
                retained = await retaining.WaitAsync(TimeSpan.FromSeconds(10));
            }
            finally
            {
                release.TrySetResult();
                await held;
                parent.Dispose();
            }
            runtime.SealApplicationAdmissionsForDrain(CancellationToken.None);
            Assert.Equal(1, runtime.SnapshotOperationAdmissions().ActiveCount);
            Assert.Equal(0, runtime.GetDrainRemainderCounts().Requests);
            var drained = runtime.WaitForAcceptedOperationsForDrainAsync();
            Assert.False(drained.IsCompleted);
            retained.Dispose();
            await drained.WaitAsync(TimeSpan.FromSeconds(10));
            Assert.Equal(0, runtime.SnapshotOperationAdmissions().ActiveCount);
        }
        finally
        {
            release.TrySetResult();
            await held;
            parent.Dispose();
            retained ??= await retaining;
            retained.Dispose();
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task BusyDrainLane_AdmissionDoesNotWaitForSealQuery(bool sealedForShutdown)
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Logging.ClearProviders();
        builder.Services.AddZLinkFramework(_ => { });
        using var host = builder.Build();
        var runtime = host.Services.GetRequiredService<ZLinkFrameworkRuntime>();
        await runtime.StartAsync(CancellationToken.None);
        if (sealedForShutdown)
            runtime.SealApplicationAdmissionsForDrain(CancellationToken.None);
        ZLinkStateLane? lane = null;
        ZLinkStateLane.WorkObserver.Value = (_, current) => lane = current;
        _ = runtime.DrainAdmission.DrainOwner;
        ZLinkStateLane.WorkObserver.Value = null;
        Assert.NotNull(lane);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var held = lane.RunAsync(async () => await release.Task);
        try
        {
            var admission = await Task.Run(() =>
                    runtime.TryEnterInboundOperation(countAsRequest: true, ownsObjectWork: false)
                )
                .WaitAsync(TimeSpan.FromSeconds(10));
            using (admission.Lease)
                Assert.Equal(!sealedForShutdown, admission.Accepted);
        }
        finally
        {
            release.SetResult();
            await held;
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Theory]
    [InlineData(false, false, false)]
    [InlineData(false, false, true)]
    [InlineData(false, true, false)]
    [InlineData(false, true, true)]
    [InlineData(true, false, false)]
    [InlineData(true, false, true)]
    [InlineData(true, true, false)]
    [InlineData(true, true, true)]
    public async Task ShutdownSeal_WaitsForReservationAndAcceptedOperation(
        bool countAsRequest,
        bool stopRuntime,
        bool acceptedBeforeSeal
    )
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Logging.ClearProviders();
        builder.Services.AddZLinkFramework(_ => { });
        using var host = builder.Build();
        var runtime = host.Services.GetRequiredService<ZLinkFrameworkRuntime>();
        await runtime.StartAsync(CancellationToken.None);
        var baseline = runtime.SnapshotOperationAdmissions();
        var paused = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        using var release = new ManualResetEventSlim();
        var stage = acceptedBeforeSeal ? "inbound-operation-counted" : "inbound-reserved";
        ZLinkStateLane.WorkObserver.Value = (currentStage, _) =>
        {
            if (currentStage != stage)
                return;
            paused.TrySetResult();
            if (!release.Wait(TimeSpan.FromSeconds(10)))
                throw new TimeoutException("The test did not release inbound admission.");
        };
        var admissionTask = Task.Run(() =>
            runtime
                .TryAdmitInboundOperationAsync(countAsRequest, ownsObjectWork: false)
                .GetAwaiter()
                .GetResult()
        );
        ZLinkStateLane.WorkObserver.Value = null;
        ZLinkFrameworkRuntime.ZLinkRuntimeOperationLease? lease = null;
        try
        {
            await paused.Task.WaitAsync(TimeSpan.FromSeconds(10));
            runtime.SealApplicationAdmissionsForDrain(CancellationToken.None);
            var drained = stopRuntime
                ? runtime.StopAsync(CancellationToken.None).AsTask()
                : runtime.WaitForAcceptedOperationsForDrainAsync();
            Assert.False(drained.IsCompleted);
            var pending = runtime.SnapshotOperationAdmissions();
            Assert.Equal(acceptedBeforeSeal ? 1 : 0, pending.ActiveCount);
            Assert.Equal(baseline.Epoch + (acceptedBeforeSeal ? 1 : 0), pending.Epoch);
            Assert.Equal(
                acceptedBeforeSeal && countAsRequest ? 1 : 0,
                runtime.GetDrainRemainderCounts().Requests
            );
            release.Set();
            var admission = (
                await admissionTask.WaitAsync(TimeSpan.FromSeconds(10))
            ).EnterInbound();
            lease = admission.Lease;
            Assert.Equal(acceptedBeforeSeal, admission.Accepted);
            if (acceptedBeforeSeal)
                Assert.False(drained.IsCompleted);
            lease.Dispose();
            await drained.WaitAsync(TimeSpan.FromSeconds(10));
            var after = runtime.SnapshotOperationAdmissions();
            Assert.Equal(0, after.ActiveCount);
            Assert.Equal(baseline.Epoch + (acceptedBeforeSeal ? 2 : 0), after.Epoch);
            output.WriteLine(
                $"accepted before seal={acceptedBeforeSeal}, stop={stopRuntime}, pending={pending}, after={after}"
            );
        }
        finally
        {
            release.Set();
            // Always retrieve and release a possibly accepted result before stopping the host.
            var admission = await admissionTask;
            lease ??= admission.EnterInbound().Lease;
            lease.Dispose();
            await runtime.StopAsync(CancellationToken.None);
        }
    }
#endif

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task NeutralAndShutdownRefusal_LeaveOperationEpochUnchanged(bool countAsRequest)
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Logging.ClearProviders();
        builder.Services.AddZLinkFramework(_ => { });
        using var host = builder.Build();
        var runtime = host.Services.GetRequiredService<ZLinkFrameworkRuntime>();
        var baseline = runtime.SnapshotOperationAdmissions();
        for (var index = 0; index < 32; index++)
        {
            var neutral = runtime.TryEnterInboundOperation(countAsRequest, ownsObjectWork: false);
            using (neutral.Lease)
            {
                Assert.True(neutral.Accepted);
                Assert.False(neutral.Lease.IsActive);
            }
        }
        Assert.Equal(baseline, runtime.SnapshotOperationAdmissions());
        await runtime.StartAsync(CancellationToken.None);
        try
        {
            runtime.SealApplicationAdmissionsForDrain(CancellationToken.None);
            for (var index = 0; index < 32; index++)
            {
                var refused = runtime.TryEnterInboundOperation(
                    countAsRequest,
                    ownsObjectWork: false
                );
                using (refused.Lease)
                    Assert.False(refused.Accepted);
            }
            await runtime
                .WaitForAcceptedOperationsForDrainAsync()
                .WaitAsync(TimeSpan.FromSeconds(10));
            Assert.Equal(baseline, runtime.SnapshotOperationAdmissions());
            Assert.Equal(0, runtime.GetDrainRemainderCounts().Requests);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(false, true)]
    [InlineData(true, false)]
    [InlineData(true, true)]
    public async Task RelocationSeal_AdmitsExistingOwnerUntilShutdownTakesOwnership(
        bool countAsRequest,
        bool ownsObjectWork
    )
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Logging.ClearProviders();
        builder.Services.AddZLinkFramework(_ => { });
        using var host = builder.Build();
        var runtime = host.Services.GetRequiredService<ZLinkFrameworkRuntime>();
        await runtime.StartAsync(CancellationToken.None);
        try
        {
            Assert.True(runtime.DrainAdmission.BeginDrain(ZLinkDrainOwner.Relocation));
            runtime.DrainAdmission.Seal();
            var inbound = runtime.TryEnterInboundOperation(countAsRequest, ownsObjectWork);
            using (inbound.Lease)
                Assert.True(inbound.Accepted);
            Assert.True(runtime.DrainAdmission.BeginDrain(ZLinkDrainOwner.Shutdown));
            Assert.True(runtime.DrainAdmission.IsSealedForShutdown);
            Assert.False(runtime.TryEnterInboundOperation(countAsRequest, ownsObjectWork).Accepted);
            // A repeated seal must retain the shutdown owner, including after takeover.
            runtime.DrainAdmission.Seal();
            Assert.True(runtime.DrainAdmission.IsSealedForShutdown);
            await runtime
                .WaitForAcceptedOperationsForDrainAsync()
                .WaitAsync(TimeSpan.FromSeconds(10));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(false, true)]
    [InlineData(true, false)]
    [InlineData(true, true)]
    public async Task ShutdownSeal_RacingAdmissionsRetainEveryAcceptedOperation(
        bool countAsRequest,
        bool ownsObjectWork
    )
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Logging.ClearProviders();
        builder.Services.AddZLinkFramework(_ => { });
        using var host = builder.Build();
        var runtime = host.Services.GetRequiredService<ZLinkFrameworkRuntime>();
        await runtime.StartAsync(CancellationToken.None);
        var leases = new List<ZLinkFrameworkRuntime.ZLinkRuntimeOperationLease>();
        try
        {
            var before = await Task.Run(Admit);
            Assert.True(before.Accepted);
            leases.Add(before.Lease);
            var start = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            var racers = Enumerable
                .Range(0, 32)
                .Select(_ =>
                    Task.Run(async () =>
                    {
                        await start.Task;
                        return Admit();
                    })
                )
                .ToArray();
            var seal = Task.Run(async () =>
            {
                await start.Task;
                runtime.SealApplicationAdmissionsForDrain(CancellationToken.None);
            });
            start.SetResult();
            var admissions = await Task.WhenAll(racers).WaitAsync(TimeSpan.FromSeconds(10));
            leases.AddRange(admissions.Where(value => value.Accepted).Select(value => value.Lease));
            await seal.WaitAsync(TimeSpan.FromSeconds(10));
            var after = Admit();
            Assert.False(after.Accepted);
            var refusal = Assert.Throws<ZLinkFrameworkException>(() =>
                runtime
                    .TryAdmitInboundOperationAsync(countAsRequest, ownsObjectWork)
                    .GetAwaiter()
                    .GetResult()
                    .Enter()
            );
            Assert.Equal(ZLinkFrameworkErrorKind.ShuttingDown, refusal.Kind);
            Assert.Equal(leases.Count, runtime.SnapshotOperationAdmissions().ActiveCount);
            Assert.Equal(
                countAsRequest ? leases.Count : 0,
                runtime.GetDrainRemainderCounts().Requests
            );
            var drained = runtime.WaitForAcceptedOperationsForDrainAsync();
            Assert.False(drained.IsCompleted);
            // Keep the operation admitted before the seal until every racer has released.
            for (var index = leases.Count - 1; index > 0; index--)
                leases[index].Dispose();
            Assert.Equal(1, runtime.SnapshotOperationAdmissions().ActiveCount);
            Assert.False(drained.IsCompleted);
            leases[0].Dispose();
            await drained.WaitAsync(TimeSpan.FromSeconds(10));
            Assert.Equal(0, runtime.SnapshotOperationAdmissions().ActiveCount);
            Assert.Equal(0, runtime.GetDrainRemainderCounts().Requests);
            output.WriteLine(
                $"before=accepted, racing accepted={leases.Count - 1}, racing refused={32 - (leases.Count - 1)}, after=ShuttingDown, drained=0"
            );
        }
        finally
        {
            foreach (var lease in leases)
                lease.Dispose();
            await runtime.StopAsync(CancellationToken.None);
        }

        ZLinkInboundOperationAdmission Admit()
        {
            var pending = runtime.TryAdmitInboundOperationAsync(countAsRequest, ownsObjectWork);
            return pending.GetAwaiter().GetResult().EnterInbound();
        }
    }
}
