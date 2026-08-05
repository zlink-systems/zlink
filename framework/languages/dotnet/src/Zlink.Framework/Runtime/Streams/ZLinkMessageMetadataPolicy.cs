namespace Zlink.Framework.Runtime.Streams;

internal sealed class ZLinkMessageMetadataPolicy(ZLinkFrameworkRegistration registration)
    : IZLinkMessageMetadataPolicy
{
    public bool CanForward(string key)
    {
        return registration.MetadataPolicy.SessionToActorKeys.Contains(key)
               || registration.MetadataPolicy.ActorToSessionKeys.Contains(key);
    }
}
