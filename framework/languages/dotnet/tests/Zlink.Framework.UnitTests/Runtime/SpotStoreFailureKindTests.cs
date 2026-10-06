using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class EntrySpotActorDispatchTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Cache_miss_Spot_operation_classifies_provider_timeout_as_unavailable(
        bool request
    )
    {
        const string spotId = "store-failure-kind";
        var (runtime, _) = await CreateStartedRuntimeAsync(
            new CapturingSpotNode(),
            includeActorFactory: false,
            locationStoreWrapper: inner => new FailedSpotReadStore(inner, spotId)
        );
        try
        {
            var client = new ZLinkSpotClient(runtime);
            var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            {
                if (request)
                    await client
                        .RequestToSpot(spotId, new ProbeRouteMessage("read"))
                        .Timeout(TimeSpan.FromMinutes(1))
                        .Async<string>();
                else
                    await client.SendToSpot(spotId, new ProbeRouteMessage("read")).Async();
            });
            Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, failure.Kind);
            Assert.IsType<TimeoutException>(failure.InnerException);
            Assert.DoesNotContain("Redis", failure.Message);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Theory]
    [InlineData("read")]
    [InlineData("write")]
    [InlineData("scan")]
    public async Task Repository_classifies_every_SPI_operation_and_retains_cause(string operation)
    {
        var cause = new TimeoutException("private provider details");
        var repository = new ZLinkProviderLocationRepository(
            new FaultingProvider(operation, cause)
        );
        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
        {
            if (operation == "read")
                await repository.ReadAuthorityAsync(
                    ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey("spot")
                );
            else if (operation == "write")
                await repository.ClaimOwnerLeaseAsync("owner", TimeSpan.FromSeconds(30));
            else
                await repository.ListMeshNodesAsync("mesh", new ZLinkPageRequest(10));
        });
        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, error.Kind);
        Assert.Same(cause, error.InnerException);
        Assert.DoesNotContain("private", error.Message);
    }

    [Fact]
    public async Task Repository_preserves_typed_validation_and_caller_cancellation()
    {
        using var cancellation = new CancellationTokenSource();
        foreach (
            var cause in new Exception[]
            {
                new ZLinkFrameworkException(ZLinkFrameworkErrorKind.ProtocolError, "typed"),
                new ArgumentException("validation"),
                new OperationCanceledException(cancellation.Token),
            }
        )
        {
            var repository = new ZLinkProviderLocationRepository(
                new FaultingProvider(
                    "read",
                    cause,
                    cause is OperationCanceledException ? cancellation.Cancel : null
                )
            );
            var error = await Record.ExceptionAsync(async () =>
                await repository.ReadAuthorityAsync(
                    ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey("spot"),
                    cancellation.Token
                )
            );
            Assert.Same(cause, error);
        }
    }

    private sealed class FaultingProvider(
        string operation,
        Exception cause,
        Action? beforeFailure = null
    ) : IZLinkLocationStore
    {
        private readonly ZLinkInMemoryProviderLocationStore _inner = new();

        public ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default
        )
        {
            if (operation != "read")
                return _inner.ReadAsync(key, cancellationToken);
            beforeFailure?.Invoke();
            return ValueTask.FromException<ZLinkStoreReadResult>(cause);
        }

        public ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default
        ) =>
            operation == "write"
                ? ValueTask.FromException<ZLinkStoreWriteResult>(cause)
                : _inner.WriteAsync(request, cancellationToken);

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default
        ) =>
            operation == "scan"
                ? ValueTask.FromException<ZLinkStoreScanResult>(cause)
                : _inner.ScanAsync(request, cancellationToken);
    }

    private sealed class FailedSpotReadStore(IZLinkLocationStore inner, string spotId)
        : IZLinkLocationStore
    {
        public ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default
        )
        {
            if (
                key
                == ZLinkProviderLocationRepository.AuthorityMetaKey(
                    ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId)
                )
            )
                return ValueTask.FromException<ZLinkStoreReadResult>(
                    new TimeoutException("private Redis command/key details")
                );
            return inner.ReadAsync(key, cancellationToken);
        }

        public ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default
        ) => inner.WriteAsync(request, cancellationToken);

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default
        ) => inner.ScanAsync(request, cancellationToken);
    }
}
