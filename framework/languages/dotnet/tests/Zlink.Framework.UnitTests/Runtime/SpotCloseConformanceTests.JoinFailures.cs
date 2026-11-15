using System.Text.Json;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class SpotCloseConformanceTests
{
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

        public async ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default
        )
        {
            if (Replay && _ready.TryGetValue(key, out var saved))
                return saved;
            var result = await _inner.ReadAsync(key, cancellationToken);
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
