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
            typeof(ZLinkSpotId)
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

    [Fact]
    public void RuntimeRegistries_UseTheirIdentifierDomainAsTheKey()
    {
        Assert.Equal(
            typeof(ZLinkActorId),
            DictionaryKeyOf(typeof(ZLinkActorSessionRegistry), "_states"));
        Assert.Equal(
            typeof(ZLinkSpotId),
            DictionaryKeyOf(typeof(ZLinkSpotNodeCatalog), "_spots"));
        Assert.Equal(
            typeof(ZLinkSpotId),
            DictionaryKeyOf(typeof(ZLinkSpotNodeCatalog), "_pending"));
        Assert.Equal(
            typeof(ZLinkSpotId),
            DictionaryKeyOf(typeof(ZLinkSpotNodeCatalog), "_closing"));
        Assert.Equal(
            typeof(ZLinkChannelName),
            FieldTypeOf(typeof(ZLinkClientServerClientRuntime), "_channelName"));
        Assert.Equal(
            typeof(ZLinkSpotId),
            FieldTypeOf(typeof(ZLinkSpotActivation), "_spotId"));
        Assert.Equal(
            typeof(ZLinkActorId),
            FieldTypeOf(typeof(ZLinkActorRuntimeState), "_actorId"));
        Assert.Equal(
            typeof(ZLinkChannelName),
            PropertyTypeOf(typeof(ZLinkSpotActivation), "RuntimeChannelName"));
        Assert.Equal(
            typeof(ZLinkMeshName),
            PropertyTypeOf(typeof(ZLinkSpotActivation), "RuntimeMeshName"));
        Assert.Equal(
            typeof(ZLinkMeshName),
            PropertyTypeOf(typeof(ZLinkActorBoundSession), "MeshName"));
    }

    private static Type DictionaryKeyOf(Type owner, string fieldName) =>
        FieldTypeOf(owner, fieldName).GetGenericArguments()[0];

    private static Type FieldTypeOf(Type owner, string fieldName) =>
        owner.GetField(fieldName, BindingFlags.Instance | BindingFlags.NonPublic)
            ?.FieldType
        ?? throw new InvalidOperationException(
            $"Field '{owner.FullName}.{fieldName}' was not found.");

    private static Type PropertyTypeOf(Type owner, string propertyName) =>
        owner.GetProperty(
                propertyName,
                BindingFlags.Instance
                | BindingFlags.Public
                | BindingFlags.NonPublic)
            ?.PropertyType
        ?? throw new InvalidOperationException(
            $"Property '{owner.FullName}.{propertyName}' was not found.");
}
