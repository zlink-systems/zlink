using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Xunit.Abstractions;
using Zlink.Framework.AspNetCore;

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
#endif

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
