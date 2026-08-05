using Zlink.Framework.Runtime.Configuration.Builders;

namespace Zlink.Framework.Runtime.Configuration;

// Test-only in-memory location store registration (spec 05-route-mesh §7, gap 90
// §12.33 / S8-08). The single-process in-memory store may be registered only from
// single-process contract tests, so this helper is kept off the public
// IZLinkFrameworkOptions surface and is reachable exclusively through
// InternalsVisibleTo("Zlink.Framework.UnitTests"). Production hosts register a
// distributed store via IZLinkFrameworkOptions.AddLocationStore or fail fast at
// host startup (see ZLinkLocationRegistrationValidator / ValidateSpotNode).
internal static class ZLinkTestLocationStores
{
    internal static void UseTestLocationStore(this IZLinkFrameworkOptions options)
    {
        ArgumentNullException.ThrowIfNull(options);

        if (options is not ZLinkFrameworkOptionsBuilder builder)
            throw new InvalidOperationException(
                "UseTestLocationStore requires the framework options created by AddZLinkFramework.");

        builder.UseTestLocationStore();
    }
}
