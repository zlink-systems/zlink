using System.Reflection;
using Zlink.Framework.Runtime.Identifiers;

namespace Zlink.Framework.UnitTests;

public sealed class RuntimeIdentifierTests
{
    [Fact]
    public void IdentifierDomains_DoNotProvideCrossDomainConversions()
    {
        var identifierTypes = new[]
        {
            typeof(ZLinkMeshName),
            typeof(ZLinkChannelName),
            typeof(ZLinkActorId),
            typeof(ZLinkSpotId),
            typeof(ZLinkSpotNodeName),
            typeof(ZLinkStreamNodeName),
            typeof(ZLinkTimerName)
        };

        Assert.Equal(identifierTypes.Length, identifierTypes.Distinct().Count());
        Assert.All(identifierTypes, type =>
            Assert.DoesNotContain(
                type.GetMethods(BindingFlags.Public | BindingFlags.Static),
                static method => method.Name is "op_Implicit" or "op_Explicit"));
    }

    [Fact]
    public void SameText_RemainsSeparateAcrossIdentifierDomains()
    {
        var actorId = ZLinkActorId.FromBoundary("same", "actorId");
        var spotId = ZLinkSpotId.FromBoundary("same", "spotId");
        var actors = new Dictionary<ZLinkActorId, string> { [actorId] = "actor" };
        var spots = new Dictionary<ZLinkSpotId, string> { [spotId] = "spot" };

        Assert.Equal("actor", actors[actorId]);
        Assert.Equal("spot", spots[spotId]);
        Assert.Equal(actorId.Value, spotId.Value);
    }
}
