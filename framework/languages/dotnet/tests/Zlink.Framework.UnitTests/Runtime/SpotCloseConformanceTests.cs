using System.Collections.Concurrent;
using System.Text.Json;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

/// <summary>
/// Runs every scenario of the cross-language Spot Close fixture
/// (framework/runtime/conformance/spot-close-v1.json) against a started .NET
/// runtime with an in-memory Location Store.
/// </summary>
public sealed class SpotCloseConformanceTests
{
    private static readonly TimeSpan Wait = TimeSpan.FromSeconds(10);

    [Fact]
    public async Task Spot_close_fixture_scenarios_hold_on_the_dotnet_runtime()
    {
        using var fixture = JsonDocument.Parse(
            await File.ReadAllTextAsync(
                Path.Combine(
                    Common.FrameworkTestEnvironment.GetRepoRoot(),
                    "framework",
                    "runtime",
                    "conformance",
                    "spot-close-v1.json"
                )
            )
        );
        var root = fixture.RootElement;
        Assert.Equal("zlink.framework.spot-close", root.GetProperty("fixture").GetString());
        Assert.Equal(1, root.GetProperty("version").GetInt32());

        // Every scenario runs; the failures of all scenarios are reported together.
        var failures = new List<string>();
        foreach (var scenario in root.GetProperty("scenarios").EnumerateArray())
        {
            var name = scenario.GetProperty("name").GetString()!;
            try
            {
                var observed = await RunScenarioAsync(name, scenario.GetProperty("given"))
                    .WaitAsync(TimeSpan.FromSeconds(60));
                AssertExpectation(name, scenario.GetProperty("expect"), observed);
            }
            catch (Exception exception)
            {
                failures.Add(
                    $"{name}: {exception.GetType().Name}: {exception.Message.ReplaceLineEndings(" ")}"
                );
            }
        }
        Assert.True(failures.Count == 0, string.Join(Environment.NewLine, failures));
    }

    [Fact]
    public async Task Context_close_completes_after_authority_release()
    {
        await using var host = await SpotCloseHost.StartAsync();
        var spot = await host.CreateSpotAsync();
        host.State.HoldOnClosing = true;
        host.State.HandlerMode = "closeAndReturn";

        Assert.Equal("handlerReply", await host.RequestAsync(spot.SpotId));
        await host.State.OnClosingEntered.Task.WaitAsync(Wait);
        var close = host.State.ContextCloseTask;
        Assert.NotNull(close);
        Assert.False(close.IsCompleted);
        Assert.Equal("Closing", await host.AuthorityAsync(spot.SpotId));

        host.State.ReleaseOnClosing.TrySetResult();
        Assert.True(await close.WaitAsync(Wait));
        Assert.Equal("Missing", await host.AuthorityAsync(spot.SpotId));
    }

    private static async Task<SpotCloseObservation> RunScenarioAsync(string name, JsonElement given)
    {
        await using (var host = await SpotCloseHost.StartAsync())
        {
            return name switch
            {
                "close-absent-incarnation-is-false" => await CloseAbsentAsync(host),
                "close-other-generation-is-invalid-operation" => await CloseOtherGenerationAsync(
                    host
                ),
                "close-with-membership-is-false-and-keeps-authority" =>
                    await CloseWithMembershipAsync(host),
                "close-after-accepted-join-observes-its-membership" =>
                    await CloseAfterAcceptedJoinAsync(host),
                "failure-before-closing-commit-keeps-authority" =>
                    await FailureBeforeClosingCommitAsync(host, given),
                "on-closing-failure-is-diagnostic-and-cleanup-continues" =>
                    await OnClosingFailureAsync(host, given),
                _ => throw new Xunit.Sdk.XunitException(
                    $"Spot Close fixture scenario '{name}' has no .NET runner."
                ),
            };
        }
    }

    private static async Task<SpotCloseObservation> CloseAbsentAsync(SpotCloseHost host)
    {
        var spot = await host.CreateSpotAsync();
        var absent = new SpotRef(
            $"absent-{Guid.NewGuid():N}",
            spot.ObjectGeneration,
            spot.MeshName,
            spot.NodeRid
        );
        Assert.Equal("Missing", await host.AuthorityAsync(absent.SpotId));
        return new SpotCloseObservation
        {
            Result = await host.CloseAsync(absent),
            OnClosingCalls = host.State.OnClosingCalls,
        };
    }

    private static async Task<SpotCloseObservation> CloseOtherGenerationAsync(SpotCloseHost host)
    {
        var spot = await host.CreateSpotAsync();
        var result = await host.CloseAsync(
            spot with
            {
                ObjectGeneration = spot.ObjectGeneration + 1,
            }
        );
        return new SpotCloseObservation
        {
            Result = result,
            Authority = await host.AuthorityAsync(spot.SpotId),
            OnClosingCalls = host.State.OnClosingCalls,
        };
    }

    private static async Task<SpotCloseObservation> CloseWithMembershipAsync(SpotCloseHost host)
    {
        var spot = await host.CreateSpotAsync();
        await host.JoinActorAsync(spot.SpotId);
        await host.State.Joined.Task.WaitAsync(Wait);
        var result = await host.CloseAsync(spot);
        return new SpotCloseObservation
        {
            Result = result,
            Authority = await host.AuthorityAsync(spot.SpotId),
            Admission = await host.AdmissionAsync(spot.SpotId),
            OnClosingCalls = host.State.OnClosingCalls,
        };
    }

    // The Join is accepted on the Spot lifecycle lane first and held in
    // OnActorJoinAsync; the manager Close submitted meanwhile must observe the
    // membership that Join decides.
    private static async Task<SpotCloseObservation> CloseAfterAcceptedJoinAsync(SpotCloseHost host)
    {
        var spot = await host.CreateSpotAsync();
        host.State.HoldJoin = true;
        await host.JoinActorAsync(spot.SpotId);
        await host.State.JoinEntered.Task.WaitAsync(Wait);
        var close = host.CloseAsync(spot)
            .ContinueWith(
                task =>
                {
                    host.State.Order.Enqueue("closeCompleted");
                    return task.Result;
                },
                TaskScheduler.Default
            );
        host.State.ReleaseJoin.TrySetResult();
        var result = await close.WaitAsync(Wait);
        return new SpotCloseObservation
        {
            Result = result,
            Authority = await host.AuthorityAsync(spot.SpotId),
            Admission = await host.AdmissionAsync(spot.SpotId),
            OnClosingCalls = host.State.OnClosingCalls,
            Order = host.State.Order.ToArray(),
        };
    }

    private static async Task<SpotCloseObservation> FailureBeforeClosingCommitAsync(
        SpotCloseHost host,
        JsonElement given
    )
    {
        Assert.Equal("ownerFenceMismatch", given.GetProperty("closingCommit").GetString());
        var spot = await host.CreateSpotAsync();
        host.Store.ConflictPutsFor = spot.SpotId;
        var result = await host.CloseAsync(spot);
        host.Store.ConflictPutsFor = null;
        return new SpotCloseObservation
        {
            Result = result,
            Authority = await host.AuthorityAsync(spot.SpotId),
            Admission = await host.AdmissionAsync(spot.SpotId),
            OnClosingCalls = host.State.OnClosingCalls,
        };
    }

    private static async Task<SpotCloseObservation> OnClosingFailureAsync(
        SpotCloseHost host,
        JsonElement given
    )
    {
        Assert.Equal("throws", given.GetProperty("onClosing").GetString());
        var spot = await host.CreateSpotAsync();
        host.State.ThrowOnClosing = true;
        var diagnostics = new ConcurrentQueue<string>();
        host.Runtime.ErrorSink.UnhandledCallbackException += exception =>
        {
            for (
                Exception? current = exception;
                current is not null;
                current = current.InnerException
            )
                if (current is SpotCloseProbeFailure)
                {
                    diagnostics.Enqueue("onClosingFailed");
                    return;
                }
        };
        var result = await host.CloseAsync(spot);
        return new SpotCloseObservation
        {
            Result = result,
            Authority = await host.AuthorityAsync(spot.SpotId),
            OnClosingCalls = host.State.OnClosingCalls,
            Diagnostics = diagnostics.ToArray(),
        };
    }

    private static void AssertExpectation(
        string scenario,
        JsonElement expect,
        SpotCloseObservation observed
    )
    {
        foreach (var property in expect.EnumerateObject())
        {
            var expected = property.Value;
            switch (property.Name)
            {
                case "result":
                    AssertValue(scenario, property.Name, expected, observed.Result);
                    break;
                case "results":
                    Assert.NotNull(observed.Results);
                    Assert.Equal(expected.GetArrayLength(), observed.Results!.Length);
                    for (var index = 0; index < observed.Results.Length; index++)
                        AssertValue(
                            scenario,
                            property.Name,
                            expected[index],
                            observed.Results[index]
                        );
                    break;
                case "authority":
                    Assert.Equal(expected.GetString(), observed.Authority);
                    break;
                case "authorityAfterFailure":
                    Assert.Equal(expected.GetString(), observed.AuthorityAfterFailure);
                    break;
                case "admission":
                    Assert.Equal(expected.GetString(), observed.Admission);
                    break;
                case "onClosingCalls":
                    Assert.Equal(expected.GetInt32(), observed.OnClosingCalls);
                    break;
                case "handlerCalls":
                    Assert.Equal(expected.GetInt32(), observed.HandlerCalls);
                    break;
                case "creationIntent":
                    Assert.Equal(expected.GetBoolean(), observed.CreationIntent);
                    break;
                case "order":
                    Assert.Equal(
                        expected.EnumerateArray().Select(static item => item.GetString()!),
                        observed.Order ?? []
                    );
                    break;
                case "diagnostics":
                    foreach (var diagnostic in expected.EnumerateArray())
                        Assert.Contains(diagnostic.GetString()!, observed.Diagnostics ?? []);
                    break;
                default:
                    throw new Xunit.Sdk.XunitException(
                        $"Scenario '{scenario}' expects unknown field '{property.Name}'."
                    );
            }
        }
    }

    private static void AssertValue(
        string scenario,
        string field,
        JsonElement expected,
        object? observed
    )
    {
        switch (expected.ValueKind)
        {
            case JsonValueKind.True:
            case JsonValueKind.False:
                Assert.True(
                    observed is bool value && value == expected.GetBoolean(),
                    $"{scenario}.{field}: expected {expected}, observed {observed}"
                );
                break;
            case JsonValueKind.String when expected.GetString() == "failure":
                Assert.True(
                    observed is Exception,
                    $"{scenario}.{field}: expected a failure, observed {observed}"
                );
                break;
            case JsonValueKind.String:
                Assert.Equal(expected.GetString(), Describe(observed));
                break;
            default:
                throw new Xunit.Sdk.XunitException(
                    $"Scenario '{scenario}' has an unsupported {field} value {expected}."
                );
        }
    }

    private static string? Describe(object? observed) =>
        observed switch
        {
            ZLinkFrameworkException framework => framework.Kind.ToString(),
            Exception exception => exception.GetType().Name,
            _ => observed?.ToString(),
        };
}

internal sealed record SpotCloseObservation
{
    public object? Result { get; init; }
    public object?[]? Results { get; init; }
    public string? Authority { get; init; }
    public string? AuthorityAfterFailure { get; init; }
    public string? Admission { get; init; }
    public int OnClosingCalls { get; init; }
    public int HandlerCalls { get; init; }
    public bool CreationIntent { get; init; }
    public string[]? Order { get; init; }
    public string[]? Diagnostics { get; init; }
}

internal sealed class SpotCloseHost : IAsyncDisposable
{
    internal const string MeshName = "spot-close";
    internal const string SpotType = "spot-close-probe";
    internal const string ActorType = "spot-close-actor";

    private readonly ServiceProvider _provider;
    private readonly IHostedService _hosted;

    private SpotCloseHost(
        ServiceProvider provider,
        IHostedService hosted,
        SpotCloseFaultStore store,
        SpotCloseProbeState state
    )
    {
        _provider = provider;
        _hosted = hosted;
        Store = store;
        State = state;
        Runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();
        Manager = provider.GetRequiredService<IZLinkSpotManager>();
    }

    internal ZLinkFrameworkRuntime Runtime { get; }
    internal IZLinkSpotManager Manager { get; }
    internal SpotCloseFaultStore Store { get; }
    internal SpotCloseProbeState State { get; }

    internal static async Task<SpotCloseHost> StartAsync()
    {
        var store = new SpotCloseFaultStore(new ZLinkInMemoryProviderLocationStore());
        var state = new SpotCloseProbeState();
        var services = new ServiceCollection();
        services.AddSingleton(state);
        services.AddTransient<SpotCloseProbeHandler>();
        services.AddScoped<SpotCloseScopedResource>();
        services.AddTransient<SpotCloseJoinHandler>();
        services.AddZLinkFramework(options =>
        {
            options.AddLocationStore(store);
            options.ConfigureLocations().PollingInterval = TimeSpan.FromMilliseconds(10);
            options
                .AddRouteMesh(MeshName)
                .Listen("tcp://127.0.0.1:0")
                .SetRoutingIdPrefix("spot-close")
                .SetActorLimit(100)
                .SetSpotLimit(100)
                .Objects()
                .Server()
                .AddEntrySpot<SpotCloseEntrySpot>()
                .AddActorFactory<SpotCloseActor, SpotCloseActorFactory>(
                    ActorType,
                    static factory => factory.DisableRelocation()
                )
                .AddSpotFactory<SpotCloseProbeSpot>(
                    SpotType,
                    static factory => factory.DisableRelocation()
                );
        });
        var provider = services.BuildServiceProvider();
        var hosted = provider
            .GetServices<IHostedService>()
            .Single(static service => service is ZLinkFrameworkHostedService);
        try
        {
            await hosted.StartAsync(CancellationToken.None);
            return new SpotCloseHost(provider, hosted, store, state);
        }
        catch
        {
            await provider.DisposeAsync();
            throw;
        }
    }

    internal async Task<SpotRef> CreateSpotAsync() =>
        (await Manager.Create(SpotType).InMesh(MeshName).Async()).Spot;

    internal async Task<object?> CloseAsync(SpotRef spot)
    {
        try
        {
            return await Manager.CloseAsync(spot);
        }
        catch (Exception exception)
        {
            return exception;
        }
    }

    // Instance-intent-less direct request to the Spot by its ID.
    internal async Task<object?> RequestAsync(string spotId)
    {
        try
        {
            _ = await _provider
                .GetRequiredService<IZLinkSpotClient>()
                .RequestToSpot(spotId, new SpotCloseProbeRequest("probe"))
                .Timeout(TimeSpan.FromSeconds(5))
                .Async<SpotCloseProbeReply>();
            return "handlerReply";
        }
        catch (ZLinkFrameworkException exception)
            when (exception.Origin == ZLinkErrorOrigin.Application)
        {
            return "handlerFailure";
        }
        catch (Exception exception)
        {
            return exception;
        }
    }

    internal async Task<string> AdmissionAsync(string spotId) =>
        await RequestAsync(spotId) is "handlerReply" ? "open" : "sealed";

    internal async Task JoinActorAsync(string spotId)
    {
        var actorId = $"spot-close-actor-{Guid.NewGuid():N}";
        _ = Assert.IsType<ZLinkActorCreateResult.Created>(
            await _provider
                .GetRequiredService<IZLinkActorManager>()
                .GetOrCreate(actorId, ActorType)
                .InMesh(MeshName)
                .Request(ZLinkMessage.Empty)
                .Timeout(TimeSpan.FromSeconds(10))
                .Async()
        );
        _ = await _provider
            .GetRequiredService<IZLinkActorClient>()
            .RequestToActor(actorId, new SpotCloseJoin(spotId))
            .Timeout(TimeSpan.FromSeconds(10))
            .Async<SpotCloseProbeReply>();
    }

    internal async Task<string> AuthorityAsync(string spotId)
    {
        var store = Runtime.Registration.Locations.ResolveStore()!;
        var read = await store.ReadAuthorityAsync(
            ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId),
            CancellationToken.None
        );
        if (read is not ZLinkAuthorityReadResult.Found found)
            return "Missing";
        Assert.True(
            ZLinkUserSpotAuthorityPayloadCodec.TryDecode(found.Snapshot.Payload.Span, out var user)
        );
        return user.State.ToString();
    }

    public async ValueTask DisposeAsync()
    {
        State.ReleaseOnClosing.TrySetResult();
        State.ReleaseJoin.TrySetResult();
        if (Runtime.IsStarted)
            await _hosted.StopAsync(CancellationToken.None);
        await _provider.DisposeAsync();
    }
}

internal sealed class SpotCloseProbeState
{
    private int _handlerCalls;
    private int _onClosingCalls;

    internal int HandlerCalls => Volatile.Read(ref _handlerCalls);
    internal int OnClosingCalls => Volatile.Read(ref _onClosingCalls);
    internal ConcurrentQueue<string> Order { get; } = new();
    internal string? HandlerMode { get; set; }
    internal Task<bool>? ContextCloseTask { get; set; }
    internal bool ThrowOnClosing { get; set; }
    internal bool HoldOnClosing { get; set; }
    internal bool HoldJoin { get; set; }

    internal TaskCompletionSource OnClosingEntered { get; } = Signal();
    internal TaskCompletionSource ReleaseOnClosing { get; } = Signal();
    internal TaskCompletionSource JoinEntered { get; } = Signal();
    internal TaskCompletionSource ReleaseJoin { get; } = Signal();
    internal TaskCompletionSource Joined { get; } = Signal();

    internal void RecordHandlerCall() => Interlocked.Increment(ref _handlerCalls);

    internal void RecordOnClosing() => Interlocked.Increment(ref _onClosingCalls);

    private static TaskCompletionSource Signal() =>
        new(TaskCreationOptions.RunContinuationsAsynchronously);
}

internal sealed class SpotCloseProbeFailure() : Exception("OnClosing probe failure.");

internal sealed record SpotCloseProbeRequest(string Marker);

internal sealed record SpotCloseProbeReply(string Marker);

internal sealed record SpotCloseJoin(string SpotId);

internal sealed class SpotCloseActor(IZLinkActorContext context) : IZLinkActor
{
    public string ActorId { get; } = context.ActorId;
    public IZLinkActorContext Context { get; } = context;
}

internal sealed class SpotCloseActorFactory : IZLinkActorFactory<SpotCloseActor>
{
    public ValueTask<SpotCloseActor> CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken = default
    ) => ValueTask.FromResult(new SpotCloseActor(context));
}

internal sealed class SpotCloseEntrySpot(IZLinkEntrySpotContext context)
    : IZLinkEntrySpot<SpotCloseActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;

    public void Configure() =>
        Context.Handlers.AddActorPacket<SpotCloseJoinHandler, SpotCloseActor>(
            nameof(SpotCloseJoin)
        );

    public ValueTask<ZLinkActorCreateResponse> OnCreateActorAsync(
        SpotCloseActor actor,
        ZLinkMessage request,
        CancellationToken cancellationToken
    ) => ValueTask.FromResult(ZLinkActorCreateResponse.Accept());

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken cancellationToken
    ) => ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept(request));

    public ValueTask OnJoinedActorAsync(
        SpotCloseActor actor,
        CancellationToken cancellationToken
    ) => ValueTask.CompletedTask;

    public ValueTask OnLeaveActorAsync(SpotCloseActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;
}

internal sealed class SpotCloseJoinHandler
    : IZLinkEntrySpotActorRequestHandler<
        SpotCloseEntrySpot,
        SpotCloseActor,
        SpotCloseJoin,
        SpotCloseProbeReply
    >
{
    public ValueTask<SpotCloseProbeReply> HandleAsync(
        SpotCloseEntrySpot spot,
        SpotCloseActor actor,
        IZLinkMessageContext context,
        SpotCloseJoin request,
        CancellationToken cancellationToken
    )
    {
        actor.Context.JoinSpot(request.SpotId).Timeout(TimeSpan.FromSeconds(10)).Defer();
        return ValueTask.FromResult(new SpotCloseProbeReply("join"));
    }
}

internal sealed class SpotCloseProbeSpot(
    IZLinkSpotContext context,
    SpotCloseProbeState state,
    SpotCloseScopedResource resource
) : IZLinkSpot<SpotCloseActor>
{
    public IZLinkSpotContext Context { get; } = context;

    internal SpotCloseProbeState State { get; } = state;

    internal SpotCloseScopedResource Resource { get; } = resource;

    public async ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken cancellationToken
    )
    {
        if (State.HoldJoin)
        {
            State.JoinEntered.TrySetResult();
            await State.ReleaseJoin.Task.WaitAsync(cancellationToken);
        }
        return ZLinkSpotActorJoinResult.Accept(request);
    }

    public ValueTask OnJoinedActorAsync(SpotCloseActor actor, CancellationToken cancellationToken)
    {
        State.Order.Enqueue("joinCompleted");
        State.Joined.TrySetResult();
        return ValueTask.CompletedTask;
    }

    public ValueTask OnLeaveActorAsync(SpotCloseActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public async ValueTask OnClosingAsync(
        ZLinkSpotClosingContext context,
        CancellationToken cleanupCancellationToken
    )
    {
        State.RecordOnClosing();
        State.Order.Enqueue("onClosing");
        if (State.HoldOnClosing)
        {
            State.OnClosingEntered.TrySetResult();
            await State.ReleaseOnClosing.Task.WaitAsync(cleanupCancellationToken);
        }
        if (State.ThrowOnClosing)
            throw new SpotCloseProbeFailure();
    }
}

internal sealed class SpotCloseProbeHandler
    : IZLinkSpotRequestHandler<SpotCloseProbeSpot, SpotCloseProbeRequest, SpotCloseProbeReply>
{
    public ValueTask<SpotCloseProbeReply> HandleAsync(
        SpotCloseProbeSpot spot,
        SpotCloseProbeRequest request,
        CancellationToken cancellationToken
    )
    {
        spot.State.RecordHandlerCall();
        switch (spot.State.HandlerMode)
        {
            case "closeAndReturn":
                spot.State.ContextCloseTask = spot.Context.CloseAsync().AsTask();
                break;
        }
        return ValueTask.FromResult(new SpotCloseProbeReply(request.Marker));
    }
}

// Provider-level Location Store that injects a lost Closing CAS.
internal sealed class SpotCloseFaultStore(IZLinkLocationStore inner) : IZLinkLocationStore
{
    internal string? ConflictPutsFor { get; set; }

    public ValueTask<ZLinkStoreReadResult> ReadAsync(
        ZLinkStoreKey key,
        CancellationToken cancellationToken = default
    ) => inner.ReadAsync(key, cancellationToken);

    public async ValueTask<ZLinkStoreWriteResult> WriteAsync(
        ZLinkStoreWriteRequest request,
        CancellationToken cancellationToken = default
    )
    {
        if (
            ConflictPutsFor is { } conflict
            && request.Mutations.Any(mutation =>
                mutation is ZLinkStoreMutation.Put put && put.Key.Value.Contains(conflict)
            )
        )
        {
            var now = await inner.ReadAsync(
                new ZLinkStoreKey("spot-close-clock"),
                cancellationToken
            );
            return new ZLinkStoreWriteResult.Conflict(
                now is ZLinkStoreReadResult.Missing missing
                    ? missing.StoreNow
                    : DateTimeOffset.UtcNow
            );
        }
        return await inner.WriteAsync(request, cancellationToken);
    }

    public ValueTask<ZLinkStoreScanResult> ScanAsync(
        ZLinkStoreScanRequest request,
        CancellationToken cancellationToken = default
    ) => inner.ScanAsync(request, cancellationToken);
}

// A scoped resource of the Spot activation scope.
internal sealed class SpotCloseScopedResource : IAsyncDisposable
{
    public ValueTask DisposeAsync() => ValueTask.CompletedTask;
}
