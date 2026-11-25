using System.Text.Json;
using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class SpotCloseConformanceTests
{
    public static IEnumerable<object[]> RouteCacheRepetitions() =>
        Enumerable.Range(0, 40).Select(index => new object[] { index });

    [Theory]
    [MemberData(nameof(RouteCacheRepetitions))]
    public async Task RemoteJoin_Unavailable_InvalidatesWarmRoute(int iteration)
    {
        using var flow = ClosingFlow(
            nameof(RemoteJoin_Unavailable_InvalidatesWarmRoute),
            true,
            iteration
        );
        var snapshot = new JoinSnapshotStore();
        await using var source = await SpotCloseHost.StartAsync(
            new SpotCloseFaultStore(snapshot),
            registerUserSpotFactory: false,
            actorRelocationEnabled: true
        );
        var actorId = await source.CreateActorAsync();
        await using var owner = await StartPeerAsync(source, actorRelocationEnabled: true);
        var spot = await owner.CreateSpotAsync();
        snapshot.SpotId = spot.SpotId;
        var rows = source.Runtime.Services.GetRequiredService<ZLinkStoreLocationResolvers>();
        var key = new ZLinkSpotLocationKey(spot.SpotId);
        Assert.NotNull(await rows.ResolveSpotRowAsync(key));
        Assert.True(await owner.Manager.CloseAsync(spot));
        var reads = snapshot.SpotReads;
        await source.JoinActorAsync(spot.SpotId, actorId);
        var first = Assert.IsType<ZLinkActorJoinCompletion.Failed>(
            await source.State.JoinCompletion.Task.WaitAsync(Wait)
        );
        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, first.Kind);
        Assert.Equal(reads, snapshot.SpotReads);
        source.State.JoinCompletion = new(TaskCreationOptions.RunContinuationsAsynchronously);
        await source.JoinActorAsync(spot.SpotId, actorId);
        var second = Assert.IsType<ZLinkActorJoinCompletion.Failed>(
            await source.State.JoinCompletion.Task.WaitAsync(Wait)
        );
        output.WriteLine(
            $"iteration={iteration} first={first.Kind} next={second.Kind} reads={reads}->{snapshot.SpotReads}"
        );
        Assert.Equal(ZLinkFrameworkErrorKind.NotFound, second.Kind);
        Assert.True(snapshot.SpotReads > reads);
    }

    [Theory]
    [MemberData(nameof(RouteCacheRepetitions))]
    public async Task ReadyResolve_OverlappingClosingObservation_DoesNotRepopulateCache(
        int iteration
    )
    {
        using var flow = ClosingFlow(
            nameof(ReadyResolve_OverlappingClosingObservation_DoesNotRepopulateCache),
            false,
            iteration
        );
        var snapshot = new JoinSnapshotStore();
        await using var host = await SpotCloseHost.StartAsync(new SpotCloseFaultStore(snapshot));
        var spot = await host.CreateSpotAsync();
        snapshot.SpotId = spot.SpotId;
        var rows = host.Runtime.Services.GetRequiredService<ZLinkStoreLocationResolvers>();
        var key = new ZLinkSpotLocationKey(spot.SpotId);
        await rows.InvalidateSpotRouteAsync(key);
        snapshot.HoldReadyRead = true;
        var resolve = rows.ResolveSpotRowAsync(key).AsTask();
        host.State.HoldOnClosing = true;
        Task<bool>? close = null;
        try
        {
            await snapshot.ReadHeld.Task.WaitAsync(Wait);
            close = host.Manager.CloseAsync(spot).AsTask();
            await host.State.OnClosingEntered.Task.WaitAsync(Wait);
            snapshot.ReleaseRead.TrySetResult();
            await resolve.WaitAsync(Wait);
            var reads = snapshot.SpotReads;
            var next = await rows.ResolveSpotRowWithStatusAsync(key);
            Assert.Equal(ZLinkLocationResolutionKind.Closing, next.Kind);
            Assert.True(snapshot.SpotReads > reads);
            output.WriteLine(
                $"iteration={iteration} stale Ready was not cached; next={next.Kind} reads={reads}->{snapshot.SpotReads}"
            );
        }
        finally
        {
            snapshot.ReleaseRead.TrySetResult();
            host.State.ReleaseOnClosing.TrySetResult();
            if (close is not null)
                Assert.True(await close.WaitAsync(Wait));
        }
    }

    [Fact]
    public async Task SameNodeActorJoin_EntrySpotIdIsNotAUserSpot()
    {
        using var flow = ClosingFlow(nameof(SameNodeActorJoin_EntrySpotIdIsNotAUserSpot), false, 0);
        await using var host = await SpotCloseHost.StartAsync();
        var actorId = await host.CreateActorAsync();
        var entrySpotId = await host.GetEntrySpotIdAsync(actorId);
        Assert.False(string.IsNullOrWhiteSpace(entrySpotId));

        await host.JoinActorAsync(entrySpotId, actorId);

        var result = Assert.IsType<ZLinkActorJoinCompletion.Failed>(
            await host.State.JoinCompletion.Task.WaitAsync(Wait)
        );
        output.WriteLine($"4510 Entry Spot JoinSpot target={entrySpotId} kind={result.Kind}");
        Assert.NotEqual(default, result.OperationId);
        Assert.Equal(ZLinkFrameworkErrorKind.NotFound, result.Kind);
        Assert.IsType<ZLinkActorCreateResult.Existing>(await host.GetExistingActorAsync(actorId));
    }

    public static IEnumerable<object[]> JoinFailureCases()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (directory is not null)
        {
            var path = Path.Combine(
                directory.FullName,
                "framework/runtime/conformance/spot-close-v1.json"
            );
            if (File.Exists(path))
            {
                using var fixture = JsonDocument.Parse(File.ReadAllText(path));
                foreach (
                    var row in fixture.RootElement.GetProperty("joinFailureCases").EnumerateArray()
                )
                    yield return
                    [
                        row.GetProperty("target").GetString()!,
                        row.GetProperty("warmRoute").GetBoolean(),
                        row.GetProperty("errorKind").GetString()!,
                    ];
                yield break;
            }
            directory = directory.Parent;
        }
        throw new FileNotFoundException("Spot Close fixture was not found.");
    }

    [Theory]
    [MemberData(nameof(JoinFailureCases))]
    public async Task DeferredJoinFailureContract(string target, bool warmRoute, string expected)
    {
        await AssertDeferredJoinFailureContract(target, warmRoute, expected);
    }

    [Fact]
    public async Task DeferredJoin_ClosingResolutionSurvivesActivationRemoval()
    {
        await AssertDeferredJoinFailureContract("closing", false, "NotFound", true);
    }

    private async Task AssertDeferredJoinFailureContract(
        string target,
        bool warmRoute,
        string expected,
        bool removeAfterResolution = false
    )
    {
        using var flow = ClosingFlow("JoinFailure-" + target, warmRoute, 0);
        var snapshot = new JoinSnapshotStore();
        await using var host = await SpotCloseHost.StartAsync(new SpotCloseFaultStore(snapshot));
        var spot = await host.CreateSpotAsync();
        snapshot.SpotId = spot.SpotId;
        Assert.Equal("Ready", await host.AuthorityAsync(spot.SpotId));
        var joinId = target == "missing" ? "missing-" + spot.SpotId : spot.SpotId;
        if (warmRoute)
        {
            var probe = await host.RequestAsync(joinId);
            if (target == "missing")
                Assert.Equal(
                    ZLinkFrameworkErrorKind.NotFound,
                    Assert.IsType<ZLinkFrameworkException>(probe).Kind
                );
            else
                Assert.Equal("handlerReply", probe);
        }
        var actorId = await host.CreateActorAsync();
        Task<bool>? closing = null;
        try
        {
            if (target == "closing")
            {
                host.State.HoldOnClosing = true;
                closing = host.Manager.CloseAsync(spot).AsTask();
                await host.State.OnClosingEntered.Task.WaitAsync(Wait);
                Assert.Equal("Closing", await host.AuthorityAsync(spot.SpotId));
                snapshot.HoldClosingRead = removeAfterResolution;
            }
            else
            {
                // Capture the public Store boundary before Close removes the target.
                snapshot.Capture = false;
                Assert.True(await host.Manager.CloseAsync(spot));
                snapshot.Replay = target == "absent";
            }
            Assert.IsType<ZLinkActorCreateResult.Existing>(
                await host.GetExistingActorAsync(actorId)
            );
            await host.JoinActorAsync(joinId, actorId);
            if (removeAfterResolution)
            {
                await snapshot.ReadHeld.Task.WaitAsync(Wait);
                Assert.False(host.State.JoinCompletion.Task.IsCompleted);
                host.State.ReleaseOnClosing.TrySetResult();
                Assert.True(await closing!.WaitAsync(Wait));
                snapshot.ReleaseRead.TrySetResult();
            }
            var result = Assert.IsType<ZLinkActorJoinCompletion.Failed>(
                await host.State.JoinCompletion.Task.WaitAsync(Wait)
            );
            output.WriteLine(
                $"4510 target={target} warm={warmRoute} expected={expected} actual={result.Kind}"
            );
            Assert.Equal(Enum.Parse<ZLinkFrameworkErrorKind>(expected), result.Kind);
        }
        finally
        {
            snapshot.Replay = false;
            snapshot.ReleaseRead.TrySetResult();
            host.State.ReleaseOnClosing.TrySetResult();
            if (closing is not null)
                Assert.True(await closing.WaitAsync(Wait));
        }
    }

    private sealed class JoinSnapshotStore : IZLinkLocationStore
    {
        private readonly IZLinkLocationStore _inner = new ZLinkInMemoryProviderLocationStore();
        private readonly System.Collections.Concurrent.ConcurrentDictionary<
            ZLinkStoreKey,
            ZLinkStoreReadResult
        > _ready = new();
        public string? SpotId { get; set; }
        public bool Capture { get; set; } = true;
        public bool Replay { get; set; }
        public bool HoldClosingRead { get; set; }
        public bool HoldReadyRead { get; set; }
        private int _spotReads;
        public int SpotReads => Volatile.Read(ref _spotReads);
        public TaskCompletionSource ReadHeld { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource ReleaseRead { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public async ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default
        )
        {
            if (Replay && _ready.TryGetValue(key, out var saved))
                return saved;
            var result = await _inner.ReadAsync(key, cancellationToken);
            if (SpotId is not null && key.Value.EndsWith("\0" + SpotId, StringComparison.Ordinal))
            {
                Interlocked.Increment(ref _spotReads);
                if (HoldReadyRead)
                {
                    HoldReadyRead = false;
                    ReadHeld.TrySetResult();
                    await ReleaseRead.Task.WaitAsync(cancellationToken);
                }
            }
            if (
                HoldClosingRead
                && key.Value.StartsWith("authority\0", StringComparison.Ordinal)
                && SpotId is not null
                && key.Value.EndsWith("\0" + SpotId, StringComparison.Ordinal)
            )
            {
                HoldClosingRead = false;
                ReadHeld.TrySetResult();
                await ReleaseRead.Task.WaitAsync(cancellationToken);
            }
            if (
                Capture
                && SpotId is not null
                && key.Value.Contains(SpotId, StringComparison.Ordinal)
            )
                _ready[key] = result;
            return result;
        }

        public ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default
        ) => _inner.WriteAsync(request, cancellationToken);

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default
        ) => _inner.ScanAsync(request, cancellationToken);
    }
}
