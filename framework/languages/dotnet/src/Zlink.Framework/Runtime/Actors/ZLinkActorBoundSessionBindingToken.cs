namespace Zlink.Framework.Runtime.Actors;

internal static class ZLinkActorBoundSessionBindingToken
{
    private const string NativePrefix = "native:";

    public static string Native(RoutingId sourceSessionRid)
    {
        return $"{NativePrefix}{sourceSessionRid.ToHex()}";
    }

    public static bool IsNative(string bindingToken)
    {
        return bindingToken.StartsWith(NativePrefix, StringComparison.Ordinal);
    }
}
