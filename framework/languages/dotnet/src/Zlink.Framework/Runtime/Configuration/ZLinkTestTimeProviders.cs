using Zlink.Framework.Runtime.Configuration.Builders;

namespace Zlink.Framework.Runtime.Configuration;

// Keeps controllable clocks off the public options contract. Runtime behavior
// still defaults to TimeProvider.System; unit tests reach this through
// InternalsVisibleTo("Zlink.Framework.UnitTests").
internal static class ZLinkTestTimeProviders
{
    internal static void UseTestTimeProvider(
        this IZLinkFrameworkOptions options,
        TimeProvider timeProvider)
    {
        ArgumentNullException.ThrowIfNull(options);
        ArgumentNullException.ThrowIfNull(timeProvider);

        if (options is not ZLinkFrameworkOptionsBuilder builder)
            throw new InvalidOperationException(
                "UseTestTimeProvider requires the framework options created by AddZLinkFramework.");

        builder.UseTestTimeProvider(timeProvider);
    }
}
