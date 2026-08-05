using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class SpotIdValidationTests
{
    [Fact]
    public void CallerProvidedEntrySpotIdIsRejectedBeforeRuntimeAdmission()
    {
        const string spotId =
            "play-entry-f67e5507-21c6-4a15-bfd1-4a240bfab371";

        var error = Assert.Throws<ZLinkFrameworkException>(
            () => ZLinkSpotId.RequireCallerProvided(spotId, nameof(spotId)));

        Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, error.Kind);
    }
}
