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
        await ZLinkSpotStartupValidator.ValidateAsync(services, Registration(typeof(RequiredSpot)));
        Assert.Equal(0, constructed);
    }

    [Fact]
    public async Task MissingRequiredDependencyFailsStartup()
    {
        await using var services = new ServiceCollection().BuildServiceProvider();
        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await ZLinkSpotStartupValidator.ValidateAsync(
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
        await ZLinkSpotStartupValidator.ValidateAsync(services, Registration(typeof(KeyedSpot)));
        await ZLinkSpotStartupValidator.ValidateAsync(services, Registration(typeof(OptionalSpot)));
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task HandlerOwnerMismatchIsAConfigurationError(bool scan)
    {
        var error = scan
            ? await Assert.ThrowsAsync<ZLinkConfigurationException>(async () =>
            {
                await using var services = new ServiceCollection().BuildServiceProvider();
                await ZLinkSpotStartupValidator.ValidateAsync(
                    services,
                    Registration(typeof(DerivedOwnerSpot))
                );
            })
            : Assert.Throws<ZLinkConfigurationException>(() =>
                ZLinkSpotDescriptorFactory.CreatePacketDescriptor(
                    typeof(BaseOwnerHandler),
                    typeof(DerivedOwnerSpot)
                )
            );
        Assert.Contains(typeof(BaseOwnerHandler).ToString(), error.Message);
        Assert.Contains(typeof(BaseOwnerSpot).ToString(), error.Message);
        Assert.Contains(typeof(DerivedOwnerSpot).ToString(), error.Message);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task InheritedAttributeOwnerIsAConfigurationError(bool scan)
    {
        var method = typeof(BaseAttributeSpot).GetMethod(nameof(BaseAttributeSpot.HandleAsync))!;
        var error = scan
            ? await Assert.ThrowsAsync<ZLinkConfigurationException>(async () =>
            {
                await using var services = new ServiceCollection().BuildServiceProvider();
                await ZLinkSpotStartupValidator.ValidateAsync(
                    services,
                    Registration(typeof(DerivedAttributeSpot))
                );
            })
            : Assert.Throws<ZLinkConfigurationException>(() =>
                ZLinkSpotDescriptorFactory.CreateAttributedRequestDescriptor(
                    typeof(DerivedAttributeSpot),
                    method,
                    null
                )
            );
        Assert.Contains(typeof(BaseAttributeSpot).ToString(), error.Message);
        Assert.Contains(typeof(DerivedAttributeSpot).ToString(), error.Message);
        ZLinkSpotDescriptorFactory.CreateAttributedRequestDescriptor(
            typeof(BaseAttributeSpot),
            method,
            null
        );
        await using var exactServices = new ServiceCollection().BuildServiceProvider();
        await ZLinkSpotStartupValidator.ValidateAsync(
            exactServices,
            Registration(typeof(BaseAttributeSpot))
        );
    }

    private class BaseAttributeSpot : IZLinkSpot
    {
        public IZLinkSpotContext Context => throw new InvalidOperationException();

        [ZLinkSpotRequest(PacketName = "owner-attribute")]
        public ValueTask<OwnerMessage> HandleAsync(OwnerMessage message) =>
            ValueTask.FromResult(message);
    }

    private sealed class DerivedAttributeSpot : BaseAttributeSpot { }

    [Fact]
    public void ExplicitActorHandlerOwnerMustMatchConcreteSpot()
    {
        var handlerType = typeof(BaseActorOwnerHandler<OwnerMessage>);
        var error = Assert.Throws<ZLinkConfigurationException>(() =>
            ZLinkSpotActorHandlerDescriptorFactory.CreatePacket(
                ZLinkSpotActorHandlerSurface.UserSpot,
                typeof(DerivedOwnerSpot),
                handlerType,
                typeof(BaseOwnerActor),
                null
            )
        );
        foreach (var type in new[] { handlerType, typeof(BaseOwnerSpot), typeof(DerivedOwnerSpot) })
            Assert.Contains(type.ToString(), error.Message);
        ZLinkSpotActorHandlerDescriptorFactory.CreatePacket(
            ZLinkSpotActorHandlerSurface.UserSpot,
            typeof(BaseOwnerSpot),
            handlerType,
            typeof(BaseOwnerActor),
            null
        );
    }

    [Fact]
    public void ExplicitActorTypeMustMatchTheDeclaredActorType()
    {
        var handlerType = typeof(BaseActorOwnerHandler<OwnerMessage>);
        var error = Assert.Throws<ZLinkConfigurationException>(() =>
            ZLinkSpotActorHandlerDescriptorFactory.CreatePacket(
                ZLinkSpotActorHandlerSurface.UserSpot,
                typeof(BaseOwnerSpot),
                handlerType,
                typeof(DerivedOwnerActor),
                null
            )
        );
        foreach (
            var type in new[] { handlerType, typeof(BaseOwnerActor), typeof(DerivedOwnerActor) }
        )
            Assert.Contains(type.ToString(), error.Message);
    }

    [Fact]
    public async Task ExactHandlerOwnerSucceedsForExplicitAndAssemblyScan()
    {
        ZLinkSpotDescriptorFactory.CreatePacketDescriptor(
            typeof(BaseOwnerHandler),
            typeof(BaseOwnerSpot)
        );
        await using var services = new ServiceCollection().BuildServiceProvider();
        await ZLinkSpotStartupValidator.ValidateAsync(
            services,
            Registration(typeof(BaseOwnerSpot))
        );
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task ExactScanOwnerTakesPrecedenceOverRegisteredSubtypes(bool otherNode)
    {
        var registration = Registration(typeof(BaseOwnerSpot));
        if (otherNode)
        {
            var other = new ZLinkSpotNodeRegistration { SpotNodeName = "other" };
            other.SpotFactories.Add(typeof(DerivedOwnerSpot));
            registration.SpotNodes.Add("other", other);
        }
        else
            registration.SpotNodes["metadata"].SpotFactories.Add(typeof(DerivedOwnerSpot));
        await using var services = new ServiceCollection().BuildServiceProvider();
        await ZLinkSpotStartupValidator.ValidateAsync(services, registration);
    }

    private class BaseOwnerSpot : IZLinkSpot
    {
        public IZLinkSpotContext Context => throw new InvalidOperationException();
    }

    private sealed class DerivedOwnerSpot : BaseOwnerSpot { }

    private class BaseOwnerActor : IZLinkActor
    {
        public IZLinkActorContext Context => throw new InvalidOperationException();
    }

    private sealed class DerivedOwnerActor : BaseOwnerActor { }

    private sealed class BaseActorOwnerHandler<TMessage>
        : IZLinkSpotActorSendHandler<BaseOwnerSpot, BaseOwnerActor, TMessage>
    {
        public ValueTask HandleAsync(
            BaseOwnerSpot spot,
            BaseOwnerActor actor,
            IZLinkMessageContext context,
            TMessage message,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;
    }

    private sealed class OwnerMessage { }

    private sealed class BaseOwnerHandler : IZLinkSpotPacketHandler<BaseOwnerSpot, OwnerMessage>
    {
        public ValueTask HandleAsync(
            BaseOwnerSpot spot,
            IZLinkMessageContext context,
            OwnerMessage message,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;
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
