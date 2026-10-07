using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Runtime.Configuration;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests;

public sealed class StartupDependencyMetadataTests
{
    [Fact]
    public async Task StartupValidationDoesNotConstructSpotOrScopedDependencies()
    {
        var constructed = 0;
        await using var services = new ServiceCollection()
            .AddScoped<Dependency>(_ =>
            {
                constructed++;
                return new Dependency();
            })
            .BuildServiceProvider();
        await ZLinkSpotStartupValidator.ValidateDependenciesAsync(
            services,
            Registration(typeof(RequiredSpot))
        );
        Assert.Equal(0, constructed);
    }

    [Fact]
    public async Task MissingRequiredDependencyFailsStartup()
    {
        await using var services = new ServiceCollection().BuildServiceProvider();
        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await ZLinkSpotStartupValidator.ValidateDependenciesAsync(
                services,
                Registration(typeof(RequiredSpot))
            )
        );
    }

    [Fact]
    public async Task KeyedAndOptionalDependenciesUseContainerMetadata()
    {
        await using var services = new ServiceCollection()
            .AddKeyedScoped<Dependency>("selected")
            .BuildServiceProvider();
        await ZLinkSpotStartupValidator.ValidateDependenciesAsync(
            services,
            Registration(typeof(KeyedSpot))
        );
        await ZLinkSpotStartupValidator.ValidateDependenciesAsync(
            services,
            Registration(typeof(OptionalSpot))
        );
    }

    private static ZLinkFrameworkRegistration Registration(Type type)
    {
        var registration = new ZLinkFrameworkRegistration();
        var node = new ZLinkSpotNodeRegistration { SpotNodeName = "metadata" };
        node.SpotFactories.Add(type);
        registration.SpotNodes.Add("metadata", node);
        return registration;
    }

    private sealed class Dependency { }

    private abstract class MetadataSpot : IZLinkSpot
    {
        public IZLinkSpotContext Context =>
            throw new InvalidOperationException("Validation must not access a Spot.");
    }

    private sealed class RequiredSpot : MetadataSpot
    {
        public RequiredSpot(IZLinkSpotContext context, Dependency dependency) =>
            throw new InvalidOperationException("Validation must not construct a Spot.");
    }

    private sealed class KeyedSpot : MetadataSpot
    {
        public KeyedSpot(
            IZLinkSpotContext context,
            [FromKeyedServices("selected")] Dependency dependency
        ) => throw new InvalidOperationException("Validation must not construct a Spot.");
    }

    private sealed class OptionalSpot : MetadataSpot
    {
        public OptionalSpot(
            IZLinkSpotContext context,
            IEnumerable<Dependency> dependencies,
            Dependency? dependency = null
        ) => throw new InvalidOperationException("Validation must not construct a Spot.");
    }
}
