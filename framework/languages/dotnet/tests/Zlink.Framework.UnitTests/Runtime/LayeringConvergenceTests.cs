using System.Reflection;
using Zlink.Framework.Runtime.Identifiers;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed class LayeringConvergenceTests
{
    [Fact]
    public void MeshMetadataPolicyBelongsToMessagingNotConcreteBackend()
    {
        Assert.Equal(
            "Zlink.Framework.Runtime.Messaging",
            typeof(ZLinkMeshMetadataCodec).Namespace);
    }

    [Fact]
    public void RelocationStateAcceptsOnlyTypedMeshIdentity()
    {
        var methods = typeof(ZLinkActorRuntimeState)
            .GetMethods(BindingFlags.Instance | BindingFlags.Public)
            .Where(static method =>
                method.Name == "MarkRelocationSessionAuthorityCommitted")
            .ToArray();

        var method = Assert.Single(methods);
        Assert.Equal(
            typeof(ZLinkMeshName),
            method.GetParameters()[3].ParameterType);
    }

    [Fact]
    public void ActorOwnershipAcceptsOnlyTypedDomainIdentity()
    {
        var methods = typeof(ZLinkActorOwnershipCoordinator)
            .GetMethods(BindingFlags.Instance | BindingFlags.Public)
            .Where(static method =>
                method.Name == "ExecuteActorClaimThenActivateAsync")
            .ToArray();

        var method = Assert.Single(methods);
        var parameters = method.GetParameters();
        Assert.Equal(typeof(ZLinkMeshName), parameters[0].ParameterType);
        Assert.Equal(typeof(ZLinkActorId), parameters[2].ParameterType);
    }
}
