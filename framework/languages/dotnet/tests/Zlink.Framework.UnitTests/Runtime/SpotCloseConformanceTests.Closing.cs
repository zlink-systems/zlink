using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class SpotCloseConformanceTests
{
    private static async Task<bool> BeginClose(SpotCloseHost host, SpotRef spot, bool self)
    {
        if (!self)
            return await host.Manager.CloseAsync(spot);
        host.State.HandlerMode = "closeAndReturn";
        Assert.Equal("handlerReply", await host.RequestAsync(spot.SpotId));
        host.State.HandlerMode = null;
        return await host.State.ContextCloseTask!;
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task HeldClosing(bool self)
    {
        for (var iteration = 0; iteration < 5; iteration++)
        {
            using var flow = ClosingFlow(nameof(HeldClosing), self, iteration);
            await using var host = await SpotCloseHost.StartAsync();
            var original = await host.CreateSpotAsync();
            host.State.HoldOnClosing = true;
            var close = BeginClose(host, original, self);
            try
            {
                await host.State.OnClosingEntered.Task.WaitAsync(Wait);
                var create = host
                    .Manager.GetOrCreate(original.SpotId, SpotCloseHost.SpotType)
                    .InMesh(SpotCloseHost.MeshName)
                    .Async()
                    .AsTask();
                Assert.False(create.IsCompleted);
                host.State.ReleaseOnClosing.TrySetResult();
                Assert.True(await close.WaitAsync(Wait));
                var next = await create.WaitAsync(Wait);
                Assert.Equal(ZLinkSpotCreateState.Created, next.State);
                Assert.NotEqual(original.ObjectGeneration, next.Spot.ObjectGeneration);
                await host.JoinActorAsync(original.SpotId);
                await host.State.Joined.Task.WaitAsync(Wait);
            }
            finally
            {
                host.State.ReleaseOnClosing.TrySetResult();
                await close.WaitAsync(Wait);
            }
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task ExistingThenClosingJoin(bool self)
    {
        for (var iteration = 0; iteration < 5; iteration++)
        {
            using var flow = ClosingFlow(nameof(ExistingThenClosingJoin), self, iteration);
            await using var host = await SpotCloseHost.StartAsync();
            var original = await host.CreateSpotAsync();
            var existing = await host
                .Manager.GetOrCreate(original.SpotId, SpotCloseHost.SpotType)
                .InMesh(SpotCloseHost.MeshName)
                .Async();
            Assert.Equal(ZLinkSpotCreateState.Existing, existing.State);
            host.State.HoldOnClosing = true;
            var close = BeginClose(host, original, self);
            try
            {
                await host.State.OnClosingEntered.Task.WaitAsync(Wait);
                await host.JoinActorAsync(original.SpotId);
                var completion = await host.State.JoinCompletion.Task.WaitAsync(Wait);
                var failed = Assert.IsType<ZLinkActorJoinCompletion.Failed>(completion);
                Assert.Equal(ZLinkFrameworkErrorKind.NotFound, failed.Kind);
            }
            finally
            {
                host.State.ReleaseOnClosing.TrySetResult();
                await close.WaitAsync(Wait);
            }
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task OldCloseAfterCompleted(bool self)
    {
        for (var iteration = 0; iteration < 5; iteration++)
        {
            using var flow = ClosingFlow(nameof(OldCloseAfterCompleted), self, iteration);
            await using var host = await SpotCloseHost.StartAsync();
            var original = await host.CreateSpotAsync();
            Assert.True(await BeginClose(host, original, self).WaitAsync(Wait));
            Assert.False(await host.Manager.CloseAsync(original));
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task SecondCloseOfClosingIncarnationJoinsInProgressCloseWhileDeleteIsPending(
        bool self
    )
    {
        for (var iteration = 0; iteration < 5; iteration++)
        {
            using var flow = ClosingFlow(
                nameof(SecondCloseOfClosingIncarnationJoinsInProgressCloseWhileDeleteIsPending),
                self,
                iteration
            );
            await using var host = await SpotCloseHost.StartAsync();
            var original = await host.CreateSpotAsync();
            host.Store.HoldAuthorityDeleteFor = original.SpotId;
            var close = BeginClose(host, original, self);
            try
            {
                await host.Store.AuthorityDeleteHeld.Task.WaitAsync(Wait);
                Assert.Null(await host.Runtime.FindAsync(original.SpotId));
                Assert.Equal(
                    1,
                    await host
                        .Runtime.GetSpotNodeRuntime(SpotCloseHost.MeshName)
                        .Catalog.CloseTransactionCountAsync()
                );
                var second = host.Manager.CloseAsync(original).AsTask();
                Assert.False(second.IsCompleted);
                Assert.False(close.IsCompleted);
                host.Store.ReleaseAuthorityDelete.TrySetResult();
                Assert.True(await second.WaitAsync(Wait));
                Assert.True(await close.WaitAsync(Wait));
                Assert.Equal(
                    0,
                    await host
                        .Runtime.GetSpotNodeRuntime(SpotCloseHost.MeshName)
                        .Catalog.CloseTransactionCountAsync()
                );
            }
            finally
            {
                host.Store.ReleaseAuthorityDelete.TrySetResult();
                await close.WaitAsync(Wait);
            }
        }
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(true, false)]
    [InlineData(false, true)]
    [InlineData(true, true)]
    public async Task SecondCloseOfClosingIncarnationJoinsInProgressCloseWhileOnClosingIsPending(
        bool self,
        bool remote
    )
    {
        using var flow = ClosingFlow(
            nameof(SecondCloseOfClosingIncarnationJoinsInProgressCloseWhileOnClosingIsPending),
            self,
            remote ? 1 : 0
        );
        await using var host = await SpotCloseHost.StartAsync();
        var original = await host.CreateSpotAsync();
        await using var source = remote ? await StartPeerAsync(host) : null;
        var caller = source ?? host;
        host.State.HoldOnClosing = true;
        var close = BeginClose(host, original, self);
        try
        {
            await host.State.OnClosingEntered.Task.WaitAsync(Wait);
            var second = caller.Manager.CloseAsync(original).AsTask();
            await Task.Delay(30);
            Assert.False(second.IsCompleted);
            Assert.False(close.IsCompleted);
            host.State.ReleaseOnClosing.TrySetResult();
            Assert.True(await second.WaitAsync(Wait));
        }
        finally
        {
            host.State.ReleaseOnClosing.TrySetResult();
            Assert.True(await close.WaitAsync(Wait));
        }
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(true, false)]
    [InlineData(false, true)]
    [InlineData(true, true)]
    public async Task BeforeClosingCAS(bool self, bool conflict)
    {
        using var flow = ClosingFlow(nameof(BeforeClosingCAS), self, conflict ? 1 : 0);
        await using var host = await SpotCloseHost.StartAsync();
        var original = await host.CreateSpotAsync();
        host.Store.HoldNextPutFor = original.SpotId;
        if (conflict)
            host.Store.ConflictPutsFor = original.SpotId;
        var close = BeginClose(host, original, self);
        try
        {
            await host.Store.PutHeld.Task.WaitAsync(Wait);
            var existing = await host
                .Manager.GetOrCreate(original.SpotId, SpotCloseHost.SpotType)
                .InMesh(SpotCloseHost.MeshName)
                .Async();
            Assert.Equal(ZLinkSpotCreateState.Existing, existing.State);
            Assert.Equal(original.ObjectGeneration, existing.Spot.ObjectGeneration);
            await host.JoinActorAsync(original.SpotId);
            await Task.Delay(30);
            Assert.False(host.State.JoinCompletion.Task.IsCompleted);
            host.Store.ReleasePut.TrySetResult();
            if (conflict)
            {
                var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                    close.WaitAsync(Wait)
                );
                Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, error.Kind);
            }
            else
                Assert.True(await close.WaitAsync(Wait));
            var completion = await host.State.JoinCompletion.Task.WaitAsync(Wait);
            if (conflict)
            {
                Assert.IsType<ZLinkActorJoinCompletion.Accepted>(completion);
                await host.State.Joined.Task.WaitAsync(Wait);
                var kept = await host
                    .Manager.GetOrCreate(original.SpotId, SpotCloseHost.SpotType)
                    .InMesh(SpotCloseHost.MeshName)
                    .Async();
                Assert.Equal(original.ObjectGeneration, kept.Spot.ObjectGeneration);
                Assert.Equal("Ready", await host.AuthorityAsync(original.SpotId));
            }
            else
                Assert.Equal(
                    ZLinkFrameworkErrorKind.NotFound,
                    Assert.IsType<ZLinkActorJoinCompletion.Failed>(completion).Kind
                );
        }
        finally
        {
            host.Store.ReleasePut.TrySetResult();
            if (conflict)
                await Record.ExceptionAsync(() => close.WaitAsync(Wait));
            else
                await close.WaitAsync(Wait);
        }
    }

    [Fact]
    public async Task SecondCloseJoinsMembershipRefusal()
    {
        await using var host = await SpotCloseHost.StartAsync();
        var original = await host.CreateSpotAsync();
        host.State.HoldJoin = true;
        await host.JoinActorAsync(original.SpotId);
        await host.State.JoinEntered.Task.WaitAsync(Wait);
        var first = host.Manager.CloseAsync(original).AsTask();
        var second = host.Manager.CloseAsync(original).AsTask();
        try
        {
            Assert.False(first.IsCompleted);
            Assert.False(second.IsCompleted);
            host.State.ReleaseJoin.TrySetResult();
            Assert.False(await first.WaitAsync(Wait));
            Assert.False(await second.WaitAsync(Wait));
            var existing = await host
                .Manager.GetOrCreate(original.SpotId, SpotCloseHost.SpotType)
                .InMesh(SpotCloseHost.MeshName)
                .Async();
            Assert.Equal(original.ObjectGeneration, existing.Spot.ObjectGeneration);
        }
        finally
        {
            host.State.ReleaseJoin.TrySetResult();
            await Task.WhenAll(first, second).WaitAsync(Wait);
        }
    }

    [Fact]
    public async Task OldClose_cannot_close_recreated_generation()
    {
        await using var host = await SpotCloseHost.StartAsync();
        var original = await host.CreateSpotAsync();
        Assert.True(await host.Manager.CloseAsync(original));
        var next = await host
            .Manager.GetOrCreate(original.SpotId, SpotCloseHost.SpotType)
            .InMesh(SpotCloseHost.MeshName)
            .Async();
        Assert.NotEqual(original.ObjectGeneration, next.Spot.ObjectGeneration);
        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            host.Manager.CloseAsync(original).AsTask()
        );
        Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, error.Kind);
        Assert.Equal("handlerReply", await host.RequestAsync(next.Spot.SpotId));
    }

    [Fact]
    public async Task OldClose_cannot_join_transaction_after_generation_recreated()
    {
        await using var host = await SpotCloseHost.StartAsync();
        var original = await host.CreateSpotAsync();
        host.Store.HoldAuthorityDeleteFor = original.SpotId;
        host.Store.HoldAfterAuthorityDelete = true;
        var close = host.Manager.CloseAsync(original).AsTask();
        try
        {
            await host.Store.AuthorityDeleteHeld.Task.WaitAsync(Wait);
            var next = await host
                .Manager.GetOrCreate(original.SpotId, SpotCloseHost.SpotType)
                .InMesh(SpotCloseHost.MeshName)
                .Async();
            Assert.NotEqual(original.ObjectGeneration, next.Spot.ObjectGeneration);
            var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                host.Manager.CloseAsync(original).AsTask()
            );
            Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, error.Kind);
        }
        finally
        {
            host.Store.ReleaseAuthorityDelete.TrySetResult();
            Assert.True(await close.WaitAsync(Wait));
        }
    }

    [Fact]
    public async Task Remote_SecondCloseOfClosingIncarnationJoinsInProgressCloseWhileDeleteIsPending()
    {
        using var flow = ClosingFlow(
            nameof(Remote_SecondCloseOfClosingIncarnationJoinsInProgressCloseWhileDeleteIsPending),
            false,
            0
        );
        await using var host = await SpotCloseHost.StartAsync();
        var original = await host.CreateSpotAsync();
        await using var source = await StartPeerAsync(host);
        host.Store.HoldAuthorityDeleteFor = original.SpotId;
        var close = source.Manager.CloseAsync(original).AsTask();
        try
        {
            await host.Store.AuthorityDeleteHeld.Task.WaitAsync(Wait);
            Assert.Null(await source.Runtime.FindAsync(original.SpotId));
            var second = source.Manager.CloseAsync(original).AsTask();
            await Task.Delay(30);
            Assert.False(second.IsCompleted);
            Assert.False(close.IsCompleted);
            host.Store.ReleaseAuthorityDelete.TrySetResult();
            Assert.True(await second.WaitAsync(Wait));
            Assert.True(await close.WaitAsync(Wait));
            Assert.False(await source.Manager.CloseAsync(original));
            var next = await host
                .Manager.GetOrCreate(original.SpotId, SpotCloseHost.SpotType)
                .InMesh(SpotCloseHost.MeshName)
                .Async();
            Assert.NotEqual(original.ObjectGeneration, next.Spot.ObjectGeneration);
            var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                source.Manager.CloseAsync(original).AsTask()
            );
            Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, error.Kind);
        }
        finally
        {
            host.Store.ReleaseAuthorityDelete.TrySetResult();
            await close.WaitAsync(Wait);
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Remote_Closing_Join_returns_NotFound(bool self)
    {
        using var flow = ClosingFlow(nameof(Remote_Closing_Join_returns_NotFound), self, 0);
        await using var source = await SpotCloseHost.StartAsync(registerUserSpotFactory: false);
        var actorId = await source.CreateActorAsync();
        await using var host = await StartPeerAsync(source);
        var original = await host.CreateSpotAsync();
        var existing = await source
            .Manager.GetOrCreate(original.SpotId, SpotCloseHost.SpotType)
            .InMesh(SpotCloseHost.MeshName)
            .Async();
        Assert.Equal(original.ObjectGeneration, existing.Spot.ObjectGeneration);
        host.State.HoldOnClosing = true;
        var close = self
            ? BeginClose(host, original, true)
            : source.Manager.CloseAsync(original).AsTask();
        try
        {
            await host.State.OnClosingEntered.Task.WaitAsync(Wait);
            await source.JoinActorAsync(original.SpotId, actorId);
            var failed = Assert.IsType<ZLinkActorJoinCompletion.Failed>(
                await source.State.JoinCompletion.Task.WaitAsync(Wait)
            );
            Assert.Equal(ZLinkFrameworkErrorKind.NotFound, failed.Kind);
        }
        finally
        {
            host.State.ReleaseOnClosing.TrySetResult();
            Assert.True(await close.WaitAsync(Wait));
        }
    }

    [Fact]
    public async Task Lifecycle_callback_queued_before_Close_commit_releases_skipped_work()
    {
        await using var host = await SpotCloseHost.StartAsync();
        var original = await host.CreateSpotAsync();
        host.Store.HoldNextPutFor = original.SpotId;
        var close = host.Manager.CloseAsync(original).AsTask();
        var skipped = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var calls = 0;
        try
        {
            await host.Store.PutHeld.Task.WaitAsync(Wait);
            var activation = host
                .Runtime.GetSpotNodeRuntime(SpotCloseHost.MeshName)
                .Catalog.Spots.Single(spot => spot.SpotId == original.SpotId);
            Assert.True(
                activation._serial.QueueLifecycle(
                    (_, _) =>
                    {
                        Interlocked.Increment(ref calls);
                        return ValueTask.CompletedTask;
                    },
                    () => skipped.TrySetResult()
                )
            );
            host.Store.ReleasePut.TrySetResult();
            Assert.True(await close.WaitAsync(Wait));
            await skipped.Task.WaitAsync(Wait);
            Assert.Equal(0, calls);
        }
        finally
        {
            host.Store.ReleasePut.TrySetResult();
            await close.WaitAsync(Wait);
        }
    }

    [Theory]
    [InlineData("external-success")]
    [InlineData("context-success")]
    [InlineData("membership")]
    [InlineData("cas-conflict")]
    [InlineData("put-exception")]
    [InlineData("delete-exception")]
    [InlineData("closing-exception")]
    [InlineData("host-shutdown")]
    public async Task Close_transaction_registry_is_empty_after_terminal_path(string path)
    {
        await using var host = await SpotCloseHost.StartAsync();
        var spot = await host.CreateSpotAsync();
        var catalog = host.Runtime.GetSpotNodeRuntime(SpotCloseHost.MeshName).Catalog;
        switch (path)
        {
            case "external-success":
            case "context-success":
                Assert.True(
                    await BeginClose(host, spot, path == "context-success").WaitAsync(Wait)
                );
                break;
            case "membership":
                await host.JoinActorAsync(spot.SpotId);
                await host.State.Joined.Task.WaitAsync(Wait);
                Assert.False(await host.Manager.CloseAsync(spot));
                break;
            case "cas-conflict":
                host.Store.ConflictPutsFor = spot.SpotId;
                var conflict = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                    host.Manager.CloseAsync(spot).AsTask()
                );
                Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, conflict.Kind);
                Assert.Equal("Ready", await host.AuthorityAsync(spot.SpotId));
                break;
            case "put-exception":
            case "delete-exception":
                host.Store.ThrowAuthorityMutationFor = spot.SpotId;
                host.Store.ThrowAuthorityDelete = path == "delete-exception";
                var failure = await Record.ExceptionAsync(() =>
                    host.Manager.CloseAsync(spot).AsTask()
                );
                Assert.NotNull(failure);
                Assert.Contains("Injected authority mutation failure", failure.ToString());
                break;
            case "closing-exception":
                host.State.ThrowOnClosing = true;
                Assert.True(await host.Manager.CloseAsync(spot));
                Assert.Equal(1, host.State.OnClosingCalls);
                break;
            case "host-shutdown":
                await host.StopAsync();
                break;
            default:
                throw new InvalidOperationException($"Unknown terminal path: {path}");
        }
        Assert.Equal(0, await catalog.CloseTransactionCountAsync());
    }

    private static TestHostMessageFlowListener ClosingFlow(
        string scenario,
        bool self,
        int iteration
    ) =>
        new(
            Path.Combine(
                Zlink.Framework.Tests.Common.FrameworkTestEnvironment.CreateTestLogDirectory(
                    scenario
                ),
                $"{self}-{iteration}.flow"
            )
        );
}
