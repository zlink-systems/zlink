using Zlink.Framework.Contracts.Messaging;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class EntrySpotActorDispatchTests
{
    [Fact]
    public async Task Local_User_Spot_OnCreate_preserves_empty_application_exception()
    {
        var (runtime, _) = await CreateStartedRuntimeAsync(
            new CapturingSpotNode(),
            userSpotType: typeof(FailingCallbackSpot),
            includeActorFactory: false
        );
        try
        {
            var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                await runtime.GetOrCreateAsync<FailingCallbackSpot>("callback-cause")
            );
            Assert.Equal(ZLinkFrameworkErrorKind.InternalFailure, failure.Kind);
            var cause = Assert.IsType<EmptyCallbackFailure>(failure.InnerException);
            Assert.Same(FailingCallbackSpot.Original, cause);
            Assert.Equal(string.Empty, cause.Message);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private sealed class EmptyCallbackFailure() : Exception(string.Empty);

    private sealed class FailingCallbackSpot(IZLinkSpotContext context) : IZLinkSpot
    {
        internal static readonly EmptyCallbackFailure Original = new();
        public IZLinkSpotContext Context { get; } = context;

        public ValueTask<ZLinkSpotCreateResponse> OnCreateAsync(
            ZLinkMessage request,
            CancellationToken cancellationToken
        ) => throw Original;
    }
}
