using System.Reflection;

namespace Zlink.Framework.Runtime.Spots;

internal static class ZLinkSpotActorContractInspector
{
    public static IEnumerable<MethodInfo> EnumerateInterfaceMethods(Type interfaceType)
    {
        foreach (var method in interfaceType.GetMethods())
            yield return method;

        foreach (var inheritedInterface in interfaceType.GetInterfaces())
        {
            foreach (var method in inheritedInterface.GetMethods())
                yield return method;
        }
    }

    public static ZLinkSpotActorContract? GetSurfaceContract(
        ZLinkSpotActorHandlerSurface surface,
        Type spotType)
    {
        var expectedDefinition = surface == ZLinkSpotActorHandlerSurface.EntrySpot
            ? typeof(IZLinkEntrySpot<>)
            : typeof(IZLinkSpot<>);
        return GetContract(spotType, expectedDefinition);
    }

    public static ZLinkSpotActorContract? GetSpotOrEntryContract(Type spotType)
    {
        return GetContract(spotType, typeof(IZLinkSpot<>), typeof(IZLinkEntrySpot<>));
    }

    public static ZLinkSpotActorContract? GetUserSpotContract(Type spotType)
    {
        return GetContract(spotType, typeof(IZLinkSpot<>));
    }

    private static ZLinkSpotActorContract? GetContract(
        Type spotType,
        Type expectedDefinition,
        Type? alternateDefinition = null)
    {
        foreach (var contract in spotType.GetInterfaces())
        {
            var definition = GetGenericDefinition(contract);
            if (definition == expectedDefinition
                || alternateDefinition is not null && definition == alternateDefinition)
                return CreateContract(contract);
        }

        return null;
    }

    private static Type? GetGenericDefinition(Type contract)
    {
        return contract.IsGenericType ? contract.GetGenericTypeDefinition() : null;
    }

    private static ZLinkSpotActorContract CreateContract(Type contract)
    {
        return new ZLinkSpotActorContract(contract, contract.GetGenericArguments()[0]);
    }
}

internal sealed record ZLinkSpotActorContract(Type ContractType, Type ActorType);
