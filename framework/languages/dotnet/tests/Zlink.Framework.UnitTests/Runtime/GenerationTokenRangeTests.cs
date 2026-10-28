using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests;

/// <summary>
/// Locks the wire schema's nonzero-u64 range (frozen at [1, 2^63-1] for
/// signed-language interop, see service-wire-v1.schema.json) against the
/// .NET opaque generation/lifecycle token issuers that draw from
/// RandomNumberGenerator. A value with the top bit set (&gt; long.MaxValue)
/// is rejected by schema-conformant peers, which was the root cause of the
/// intermittent user-spot-join-dotnet-java "previous Actor membership is
/// invalid" failure.
///
/// Deliberately not covered here:
/// - ZLinkChannelBundleFactory.CreateLifecycleGeneration was already
///   schema-conformant before this change (rejection-samples on
///   `value is 0 or > long.MaxValue`); it needed no fix, so it is not
///   re-asserted here as a regression guard on unchanged code.
/// - ZLinkDeferredActorJoin.CreateOperationId is out of scope: it fills
///   ZLinkActorJoinOperationId, which serializes as the "operation-id"
///   struct (two plain "u64" fields with only a not-both-zero constraint,
///   not "nonzero-u64" — schema line ~4340), so full-width halves are
///   schema-conformant and must not be masked.
/// - ZlinkStreamFlowId.Create is out of scope: it produces a UUIDv7-style
///   string, not a u64 wire field.
/// </summary>
public sealed class GenerationTokenRangeTests
{
    private const int Iterations = 1_000;

    [Fact]
    public async Task ManagedMeshNodeLifecycleTokenStaysWithinNonzeroU64Range()
    {
        using var context = Systems.Zlink.Zlink.CreateContext();
        for (var i = 0; i < Iterations; i++)
        {
            await using var node = new ZLinkManagedMeshNode(context, "generation-range");
            var value = node.Status().LifecycleGeneration;
            Assert.NotEqual(0UL, value);
            Assert.True(
                value <= long.MaxValue,
                $"{nameof(ZLinkManagedMeshNode)} issued {value:X16}, "
                    + "which has the top bit set and violates the nonzero-u64 "
                    + "wire bound of [1, 2^63-1]."
            );
        }
    }
}
