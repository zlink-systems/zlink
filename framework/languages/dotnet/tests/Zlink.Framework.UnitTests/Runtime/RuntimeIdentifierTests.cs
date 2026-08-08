using System.Reflection;
using Zlink.Framework.Runtime.Backend.DotNet;
using Zlink.Framework.Runtime.Backend.DotNet.Wrappers;
using Zlink.Framework.Runtime.Channels;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Identifiers;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;
using Zlink.Framework.Runtime.Streams;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Service;

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
            typeof(ZLinkChannelName),
            FieldTypeOf(
                typeof(ZLinkAutomaticFanoutSubscriberRuntime),
                "_channelName"));
        Assert.Equal(
            typeof((RoutingId, ulong)),
            DictionaryKeyOf(
                typeof(ZLinkAutomaticFanoutSubscriberRuntime),
                "_connections"));
        Assert.Equal(
            typeof(ZLinkChannelName),
            PropertyTypeOf(
                typeof(ZLinkClientServerServerIdentity),
                "ChannelName"));
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
        Assert.Equal(
            typeof(ZLinkActorId),
            DictionaryKeyOf(typeof(ZLinkSpotSerialExecutor), "_actorLanes"));
        Assert.Equal(
            typeof(ZLinkTimerName),
            DictionaryKeyOf(typeof(ZLinkSpotSerialExecutor), "_timerLanes"));
        Assert.Equal(
            typeof(ZLinkChannelName),
            DictionaryPropertyKeyOf(typeof(ZLinkFrameworkComponentState), "SubscriberBundles"));
        Assert.Equal(
            typeof(ZLinkChannelName),
            DictionaryPropertyKeyOf(typeof(ZLinkFrameworkComponentState), "PublisherBundles"));
        Assert.Equal(
            typeof(ZLinkChannelName),
            DictionaryPropertyKeyOf(
                typeof(ZLinkFrameworkComponentState),
                "AutomaticFanoutSubscriberRuntimes"));
        Assert.Equal(
            typeof(ZLinkChannelName),
            DictionaryPropertyKeyOf(
                typeof(ZLinkFrameworkComponentState),
                "ClientServerClientBundles"));
        Assert.Equal(
            typeof(ZLinkChannelName),
            DictionaryPropertyKeyOf(
                typeof(ZLinkFrameworkComponentState),
                "ClientServerClientRuntimes"));
        Assert.Equal(
            typeof(ZLinkChannelName),
            DictionaryPropertyKeyOf(
                typeof(ZLinkFrameworkComponentState),
                "ClientServerServerBundles"));
        Assert.Equal(
            typeof(ZLinkSpotNodeName),
            DictionaryPropertyKeyOf(typeof(ZLinkFrameworkComponentState), "SpotNodes"));
        Assert.Equal(
            typeof(ZLinkChannelName),
            DictionaryPropertyKeyOf(
                typeof(ZLinkFrameworkComponentState),
                "RouteMeshNodesByChannel"));
        Assert.Equal(
            typeof(ZLinkStreamNodeName),
            DictionaryPropertyKeyOf(typeof(ZLinkFrameworkComponentState), "StreamNodes"));
        Assert.Equal(
            typeof(ZLinkActorId),
            DictionaryKeyOf(typeof(ZLinkActorOwnershipCoordinator), "_actors"));
        Assert.Equal(
            typeof(ZLinkActorId),
            DictionaryKeyOf(typeof(ZLinkManagedMeshNode), "_actors"));
        Assert.Equal(
            typeof(ZLinkSpotId),
            DictionaryKeyOf(typeof(ZLinkManagedMeshNode), "_spots"));
        Assert.Equal(
            typeof(ZLinkChannelName),
            DictionaryKeyOf(typeof(ZLinkManagedMeshNode), "_channels"));
        Assert.Equal(
            typeof(ZLinkChannelName),
            DictionaryKeyOf(typeof(ZLinkManagedSpot), "_subscriptions"));
        Assert.Equal(
            typeof(ZLinkActorId),
            NestedDictionaryKeyOf(typeof(ZLinkManagedStreamSessionService), "_bindings"));
        Assert.Equal(
            typeof(ZLinkChannelName),
            DictionaryKeyOf(typeof(ZLinkClientServerRuntimeService), "_sequences"));
        Assert.Equal(
            typeof(ZLinkChannelName),
            DictionaryKeyOf(typeof(ZLinkSpotNodeBundleRegistry), "_publisherBundles"));
        Assert.Equal(
            typeof(ZLinkChannelName),
            DictionaryKeyOf(typeof(ZLinkMeshChannelSelection), "_plans"));
        Assert.Equal(
            typeof(ZLinkChannelName),
            HashSetElementTypeOf(
                typeof(ZLinkMeshChannelSelection),
                "_declaredChannels"));
        Assert.Equal(
            typeof(ZLinkChannelName),
            HashSetElementTypeOf(
                typeof(ZLinkFanoutRuntimeService),
                "_automaticChannels"));
        Assert.Equal(
            typeof(ZLinkChannelName),
            DictionaryKeyOf(typeof(ZLinkFanoutRuntimeService), "_states"));
        Assert.Equal(
            typeof(ZLinkChannelName),
            DictionaryKeyOf(typeof(ZLinkFanoutRuntimeService), "_observers"));
        Assert.Equal(
            typeof(ZLinkActorId),
            DictionaryKeyOf(typeof(ZLinkSessionActorCoordinator), "_actorOperationGates"));
        Assert.Equal(
            typeof(ZLinkActorId),
            DictionaryKeyOf(typeof(ZLinkSpotActorMembership), "_actorsById"));
        Assert.Equal(
            typeof(ZLinkActorId),
            DictionaryKeyOf(typeof(ZLinkSpotActivation), "_actorsLeavingForEntrySpot"));
        Assert.Equal(
            typeof(ZLinkActorId),
            DictionaryKeyOf(typeof(ZLinkMeshNodeRouteDispatcher), "_orderedActorRelayTails"));
        Assert.Equal(
            typeof(RoutingId),
            DictionaryKeyOf(typeof(ZLinkActorBoundSessionRegistry), "_entries"));
        Assert.Equal(
            typeof(ZLinkSpotId),
            DictionaryKeyOf(typeof(ZLinkSpotLocationLifecycle), "_spots"));
        Assert.Equal(
            typeof(ZLinkMeshName),
            FieldTypeOf(typeof(ZLinkMeshNodeRouteDispatcher), "_meshName"));
        Assert.Equal(
            typeof(ZLinkSpotId),
            DictionaryKeyOf(typeof(ZLinkMeshDispatchPump), "_spots"));
        Assert.Equal(
            typeof(ZLinkActorId),
            DictionaryKeyOf(typeof(ZLinkFrameworkRuntime), "_remoteFrameChains"));
        Assert.Equal(
            typeof(ZLinkSpotId),
            DictionaryKeyOf(typeof(ZLinkSpotSubscriptionTracker), "_targets"));
        Assert.Equal(
            typeof(ZLinkMeshName),
            DictionaryKeyOf(typeof(ZLinkRouteMeshRuntimeService), "_sequences"));
        Assert.Equal(
            typeof(ZLinkMeshName),
            DictionaryKeyOf(typeof(ZLinkRouteMeshRuntimeService), "_monitorHubs"));
        Assert.Equal(
            typeof(RoutingId),
            DictionaryKeyOf(typeof(ZLinkStreamSessionTable), "_sessions"));
        Assert.Equal(
            typeof(RoutingId),
            DictionaryKeyOf(typeof(ZLinkStreamSessionTable), "_sessionCreations"));
        Assert.Equal(
            typeof(ZLinkActorId),
            DictionaryPropertyKeyOf(
                typeof(TargetStage),
                "ActorTargetAuthorityOwnerGenerations"));
        Assert.Equal(
            typeof(ZLinkActorId),
            PropertyTypeOf(typeof(ZLinkSessionBindingKey), "ActorId"));
        Assert.Equal(
            typeof(ZLinkMeshName),
            PropertyTypeOf(typeof(ZLinkSessionBindingRoute), "MeshName"));
        Assert.Equal(
            typeof(ZLinkMeshName),
            PropertyTypeOf(typeof(ZLinkSessionBindingIdentity), "MeshName"));
        Assert.Equal(
            typeof(ZLinkMeshName),
            DictionaryKeyOf(typeof(ZLinkBoundSessionService), "_submitters"));
        Assert.Equal(
            typeof((ZLinkRouteHandlerChannelKey, ZLinkMessageKind, string)),
            DictionaryKeyOf(typeof(ZLinkRouteHandlerRegistry), "_handlers"));
        Assert.Equal(
            typeof(RoutingId),
            DictionaryKeyOf(typeof(ZLinkAutoConnectReconciler), "_meshTargets"));
        Assert.Equal(
            typeof(RoutingId),
            HashSetElementTypeOf(
                typeof(ZLinkAutoConnectReconciler),
                "_retainedMemberRids"));
        Assert.Equal(
            typeof(ZLinkChannelName),
            DictionaryKeyOf(typeof(ZLinkAutoConnectReconciler), "_pendingChannelWeights"));
        Assert.Equal(
            typeof(ZLinkMeshName),
            ParameterTypeOf(typeof(ZLinkActorBoundSessionCoordinator), "CreateSubmitter"));
        Assert.Equal(
            typeof(ZLinkActorId),
            ParameterTypeOf(typeof(ZLinkActorSessionRegistry), "GetOrCreate"));
        Assert.Equal(
            typeof(ZLinkActorId),
            ParameterTypeOf(typeof(ZLinkActorSessionRegistry), "TryGet"));
        Assert.Equal(
            typeof(ZLinkActorId),
            ParameterTypeOf(typeof(ZLinkActorSessionRegistry), "RemoveIfCurrent"));
        Assert.Equal(
            typeof(ZLinkMeshName),
            ParameterTypeOf(typeof(ZLinkSpotLocationLifecycle), "ClaimAsync"));
        Assert.Equal(
            typeof(ZLinkSpotId),
            ParameterTypeOf(typeof(ZLinkSpotLocationLifecycle), "TryGetTrackedGeneration"));
        Assert.Equal(
            typeof(ZLinkMeshName),
            ParameterTypeOf(typeof(ZLinkSpotLocationLifecycle), "TrackRelocatedAsync"));
        Assert.Equal(
            typeof(ZLinkMeshName),
            ParameterTypeOf(typeof(ZLinkSpotLocationLifecycle), "ReleaseAsync"));
        Assert.True(
            HasMethodParameterType(
                typeof(IZLinkActorLocationLifecycle),
                "ExecuteActorClaimThenActivateAsync",
                0,
                typeof(ZLinkMeshName)));
        Assert.True(
            HasMethodParameterType(
                typeof(IZLinkActorLocationLifecycle),
                "ExecuteActorClaimThenActivateAsync",
                2,
                typeof(ZLinkActorId)));
        Assert.True(
            HasMethodParameterType(
                typeof(IZLinkActorLocationLifecycle),
                "PublishActorRefAsync",
                0,
                typeof(ZLinkActorId)));
        Assert.True(
            HasMethodParameterType(
                typeof(IZLinkActorLocationLifecycle),
                "ReleaseActorAsync",
                0,
                typeof(ZLinkActorId)));
        Assert.True(
            HasMethodParameterType(
                typeof(ZLinkActorOwnershipCoordinator),
                "ClaimActorCoreAsync",
                0,
                typeof(ZLinkMeshName)));
        Assert.True(
            HasMethodParameterType(
                typeof(ZLinkActorOwnershipCoordinator),
                "ClaimActorCoreAsync",
                2,
                typeof(ZLinkActorId)));
        Assert.True(
            HasMethodParameterType(
                typeof(ZLinkActorOwnershipCoordinator),
                "RenewOwnedActorAsync",
                0,
                typeof(ZLinkActorId)));
        Assert.True(
            HasMethodParameterType(
                typeof(ZLinkActorOwnershipCoordinator),
                "ReleaseTrackedActorAsync",
                0,
                typeof(ZLinkActorId)));
        Assert.True(
            HasMethodParameterType(
                typeof(ZLinkActorOwnershipCoordinator),
                "UpdateTrackedSnapshot",
                0,
                typeof(ZLinkActorId)));
        Assert.Contains(
            typeof(ZLinkActorId),
            typeof(ZLinkActorRuntimeState)
                .GetConstructors(
                    BindingFlags.Instance
                    | BindingFlags.Public
                    | BindingFlags.NonPublic)
                .Select(static constructor =>
                    constructor.GetParameters().First().ParameterType));
    }

    private static Type DictionaryKeyOf(Type owner, string fieldName) =>
        FieldTypeOf(owner, fieldName).GetGenericArguments()[0];

    private static Type NestedDictionaryKeyOf(Type owner, string fieldName) =>
        FieldTypeOf(owner, fieldName)
            .GetGenericArguments()[1]
            .GetGenericArguments()[0];

    private static Type HashSetElementTypeOf(Type owner, string fieldName) =>
        FieldTypeOf(owner, fieldName).GetGenericArguments()[0];

    private static Type DictionaryPropertyKeyOf(Type owner, string propertyName) =>
        PropertyTypeOf(owner, propertyName).GetGenericArguments()[0];

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

    private static Type ParameterTypeOf(Type owner, string methodName) =>
        owner.GetMethod(
                methodName,
                BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic)
            ?.GetParameters()
            .FirstOrDefault()
            ?.ParameterType
        ?? throw new InvalidOperationException(
            $"Method '{owner.FullName}.{methodName}' was not found.");

    private static bool HasMethodParameterType(
        Type owner,
        string methodName,
        int parameterIndex,
        Type parameterType) =>
        owner.GetMethods(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic)
            .Where(method => method.Name == methodName)
            .Select(method => method.GetParameters())
            .Any(parameters =>
                parameters.Length > parameterIndex
                && parameters[parameterIndex].ParameterType == parameterType);
}
