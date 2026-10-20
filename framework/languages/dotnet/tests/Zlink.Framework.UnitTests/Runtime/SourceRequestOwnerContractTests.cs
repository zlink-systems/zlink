using System.Reflection;
using System.Text.Json;
using Microsoft.Extensions.Logging;
using Zlink.Framework.Runtime.Backend.DotNet;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests.Runtime;

/// <summary>
/// Cross-language source request owner contract (fixture
/// framework/runtime/protocol/fixtures/source-request-owner.json): the first accepting owner reads
/// OwnerId and LeaseGeneration from the admitted peer's descriptor, from the polled descriptors
/// first and the Store once otherwise, and ends the request Unavailable without a live descriptor.
/// </summary>
public sealed class SourceRequestOwnerContractTests
{
    private static readonly TimeSpan PollingInterval = TimeSpan.FromSeconds(10);
    private static readonly TimeSpan LeaseTtl = TimeSpan.FromSeconds(1);

    [Fact]
    public async Task SourceRequestOwnerReportsStoreUnavailable()
    {
        var error = new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.Unavailable,
            "descriptor Store offline",
            innerException: new IOException("connection closed")
        );
        var store = SourceOwnerStore.Create(
            [],
            new ZLinkLocationOwnerToken("owner", 1),
            TimeProvider.System
        );
        ((SourceOwnerStore)(object)store).DescriptorError = error;
        var options = new ZLinkLocationOptions();
        var leases = new ZLinkOwnerLeaseTracker(store, options);
        var logger = new SourceLogger();
        var flow = new ZLinkMessageFlowTracer(new ZLinkDispatchOptionsModel(), logger);
        var resolver = new ZLinkRequestSourceFenceResolver(
            new ZLinkStoreLocationResolvers(store, leases, options: options),
            leases,
            "mesh",
            flow
        );
        Assert.Null(
            await resolver.AcceptAsync(null, RoutingId.From("sender"), 1, CancellationToken.None)
        );
        Assert.Contains(
            logger.Fields,
            field => field.Key == "error_type" && (string?)field.Value == nameof(IOException)
        );
        Assert.Contains(
            logger.Fields,
            field => field.Key == "error_message" && (string?)field.Value == "connection closed"
        );
    }

    private sealed class SourceLogger : ILogger
    {
        internal IReadOnlyList<KeyValuePair<string, object?>> Fields { get; private set; } = [];

        public IDisposable? BeginScope<TState>(TState state)
            where TState : notnull => null;

        public bool IsEnabled(LogLevel logLevel) => true;

        public void Log<TState>(
            LogLevel logLevel,
            EventId eventId,
            TState state,
            Exception? exception,
            Func<TState, Exception?, string> formatter
        ) =>
            Fields = state is IEnumerable<KeyValuePair<string, object?>> fields
                ? fields.ToArray()
                : [];
    }

    [Fact]
    public async Task SourceRequestOwnerPropagatesProgrammingError()
    {
        var error = new InvalidOperationException("broken descriptor reader");
        var store = SourceOwnerStore.Create(
            [],
            new ZLinkLocationOwnerToken("owner", 1),
            TimeProvider.System
        );
        ((SourceOwnerStore)(object)store).DescriptorError = error;
        var options = new ZLinkLocationOptions();
        var leases = new ZLinkOwnerLeaseTracker(store, options);
        var resolver = new ZLinkRequestSourceFenceResolver(
            new ZLinkStoreLocationResolvers(store, leases, options: options),
            leases,
            "mesh"
        );
        Assert.Same(
            error,
            await Assert.ThrowsAsync<InvalidOperationException>(async () =>
                await resolver.AcceptAsync(
                    null,
                    RoutingId.From("sender"),
                    1,
                    CancellationToken.None
                )
            )
        );
    }

    public static TheoryData<string> Cases()
    {
        var data = new TheoryData<string>();
        foreach (var scenario in Fixture().RootElement.GetProperty("cases").EnumerateArray())
            data.Add(scenario.GetProperty("name").GetString()!);
        return data;
    }

    [Theory]
    [MemberData(nameof(Cases))]
    public async Task SourceRequestOwner(string name)
    {
        using var fixture = Fixture();
        var root = fixture.RootElement;
        var scenario = root.GetProperty("cases")
            .EnumerateArray()
            .Single(value => value.GetProperty("name").GetString() == name);
        var meshName = root.GetProperty("meshName").GetString()!;
        var rid = RoutingId.From(root.GetProperty("peer").GetProperty("nodeRid").GetString()!);
        var nodeGeneration = root.GetProperty("peer").GetProperty("nodeGeneration").GetUInt64();
        var ownerId = root.GetProperty("descriptor").GetProperty("ownerId").GetString()!;
        var leaseGeneration = root.GetProperty("descriptor")
            .GetProperty("leaseGeneration")
            .GetInt64();

        var time = new ManualTimeProvider();
        var descriptor = new ZLinkMeshNodeDescriptor(
            meshName,
            rid,
            nodeGeneration,
            1,
            "tcp://127.0.0.1:7000",
            new Dictionary<string, int>(),
            "plain",
            ownerId,
            leaseGeneration,
            time.GetUtcNow()
        );
        var store = SourceOwnerStore.Create(
            scenario.GetProperty("stored").GetBoolean() ? [descriptor] : [],
            new ZLinkLocationOwnerToken(ownerId, leaseGeneration),
            time
        );
        var options = new ZLinkLocationOptions { PollingInterval = PollingInterval };
        var leases = new ZLinkOwnerLeaseTracker(store, options, time);
        var peers = new ZLinkStoreLocationResolvers(store, leases, options: options);
        // The pump does not use its MeshNode until it is started.
        var pump = new ZLinkMeshDispatchPump(null!, new ZLinkMeshCompletionTable());
        pump.SetRequestSourceAcceptance(
            new ZLinkRequestSourceFenceResolver(peers, leases, meshName)
        );
        var expected = new ZLinkServiceWireCodec.RequestSourceFence(
            ownerId,
            (ulong)leaseGeneration,
            rid,
            nodeGeneration
        );
        if (scenario.GetProperty("polled").GetBoolean())
        {
            // The auto-connect poll lists live descriptors and keeps the sender's values.
            Assert.Single(await peers.ListLiveMeshNodesAsync(meshName));
            pump.ObserveRequestSourceFence(expected);
            ((SourceOwnerStore)(object)store).DescriptorReads = 0;
        }
        if (!scenario.GetProperty("leaseLive").GetBoolean())
            time.Advance(LeaseTtl * 2);

        var recorded = await pump.AcceptRequestSourceAsync(
            rid,
            nodeGeneration,
            CancellationToken.None
        );

        Assert.Equal(
            scenario.GetProperty("storeDescriptorReads").GetInt32(),
            ((SourceOwnerStore)(object)store).DescriptorReads
        );
        if (scenario.GetProperty("result").GetString() == "accepted")
        {
            Assert.Equal(expected, recorded);
            // The Store answer joins the polled descriptors, so the next request does not read.
            Assert.Equal(expected, pump.ResolveRequestSourceFence(rid, nodeGeneration));
        }
        else
        {
            Assert.Null(recorded);
            var unavailable = ZLinkRequestSourceFenceResolver.Unavailable(rid);
            Assert.Equal(
                root.GetProperty("unavailable").GetProperty("errorKind").GetString(),
                unavailable.Kind.ToString()
            );
        }
    }

    private static JsonDocument Fixture() =>
        JsonDocument.Parse(
            File.ReadAllText(
                Path.Combine(
                    Common.FrameworkTestEnvironment.GetRepoRoot(),
                    "framework/runtime/protocol/fixtures/source-request-owner.json"
                )
            )
        );

    internal class SourceOwnerStore : DispatchProxy
    {
        private IReadOnlyList<ZLinkMeshNodeDescriptor> _descriptors = [];
        private ZLinkLocationOwnerToken _lease;
        private TimeProvider _time = TimeProvider.System;
        private DateTimeOffset _issuedAt;

        internal int DescriptorReads { get; set; }
        internal Exception? DescriptorError { get; set; }

        internal static IZLinkLocationRepository Create(
            IReadOnlyList<ZLinkMeshNodeDescriptor> descriptors,
            ZLinkLocationOwnerToken lease,
            TimeProvider time
        )
        {
            var proxy = Create<IZLinkLocationRepository, SourceOwnerStore>();
            var store = (SourceOwnerStore)(object)proxy;
            store._descriptors = descriptors;
            store._lease = lease;
            store._time = time;
            store._issuedAt = time.GetUtcNow();
            return proxy;
        }

        protected override object? Invoke(MethodInfo? targetMethod, object?[]? args)
        {
            switch (targetMethod?.Name)
            {
                case nameof(IZLinkLocationRepository.ListMeshNodesAsync):
                    DescriptorReads++;
                    if (DescriptorError is { } error)
                        throw error;
                    return ValueTask.FromResult(
                        new ZLinkLocationPage<ZLinkMeshNodeDescriptor>(_descriptors, null)
                    );
                case nameof(IZLinkLocationRepository.ReadOwnerLeaseAsync):
                    return ValueTask.FromResult<ZLinkOwnerLeaseReadResult>(
                        (string?)args![0] == _lease.OwnerId
                            ? new ZLinkOwnerLeaseReadResult.Found(
                                _lease,
                                _issuedAt + LeaseTtl,
                                _time.GetUtcNow()
                            )
                            : new ZLinkOwnerLeaseReadResult.Missing()
                    );
                default:
                    throw new NotSupportedException(targetMethod?.Name);
            }
        }
    }
}
