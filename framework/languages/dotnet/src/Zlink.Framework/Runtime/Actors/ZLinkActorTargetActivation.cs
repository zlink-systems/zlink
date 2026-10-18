namespace Zlink.Framework.Runtime.Actors;

internal static class ZLinkActorTargetActivation
{
    internal static async ValueTask CompleteAsync(
        Func<ValueTask> clearPublication,
        Func<ValueTask> normalizePrefix,
        Func<ValueTask> completeJoin,
        Func<ValueTask> publishSessionRoute,
        Func<ValueTask> openAdmission
    )
    {
        await clearPublication().ConfigureAwait(false);
        await normalizePrefix().ConfigureAwait(false);
        await completeJoin().ConfigureAwait(false);
        await publishSessionRoute().ConfigureAwait(false);
        await openAdmission().ConfigureAwait(false);
    }
}
