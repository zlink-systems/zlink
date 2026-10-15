namespace Zlink.Framework.Runtime.Actors;

internal static class ZLinkActorBoundSessionBindingToken
{
    private const string NativePrefix = "native:";

    internal static string Canonical(
        RoutingId ownerNodeRid,
        ulong ownerNodeGeneration,
        string ownerId,
        ulong ownerLeaseGeneration,
        RoutingId sessionRid,
        ulong bindingGeneration
    )
    {
        var bytes =
            Systems.Zlink.Framework.Runtime.Protocol.ServiceWireCodec.EncodeRelocationSessionBinding(
                new(
                    new(ZLinkCanonicalActorRelocationWriter.ParticipantId),
                    new(ownerNodeRid.ToBytes().ToArray()),
                    new(ownerNodeGeneration),
                    new(ownerId),
                    new(ownerLeaseGeneration),
                    new(sessionRid.ToBytes().ToArray()),
                    new(bindingGeneration)
                ),
                Systems.Zlink.Framework.Runtime.Protocol.ServiceWireCodec.DecodeContext.Empty
            );
        return Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(bytes));
    }

    public static string Native(RoutingId sourceSessionRid)
    {
        return $"{NativePrefix}{sourceSessionRid.ToHex()}";
    }

    public static bool IsNative(string bindingToken)
    {
        return bindingToken.StartsWith(NativePrefix, StringComparison.Ordinal);
    }
}
