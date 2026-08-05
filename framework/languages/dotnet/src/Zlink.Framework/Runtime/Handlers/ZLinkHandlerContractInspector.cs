namespace Zlink.Framework.Runtime.Handlers;

internal static class ZLinkHandlerContractInspector
{
    public static IEnumerable<(Type Definition, Type[] Arguments)> EnumerateGenericInterfaces(
        Type handlerType)
    {
        foreach (var implemented in handlerType.GetInterfaces())
            if (implemented.IsGenericType)
                yield return (implemented.GetGenericTypeDefinition(), implemented.GetGenericArguments());
    }
}
