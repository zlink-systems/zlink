namespace Zlink.Framework.Runtime.Streams;

internal interface IZLinkBoundSessionService
{
    IZLinkBoundSession Create(string actorId);

    ValueTask ResetAsync();
}
