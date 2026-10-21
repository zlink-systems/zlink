using Zlink.Framework.Runtime.Actors;

namespace Zlink.Framework.Runtime.Actors;

// State-only tests model FIFO acceptance by transferring each frame to this
// caller-owned collection. Runtime callers always use the actual Actor mailbox.
internal static class ActorHandoffTestRestoration
{
    internal static IReadOnlyList<ZLinkActorHandoffFrame> AbortCapture(
        this ZLinkActorHandoffState handoff
    ) => Collect(handoff, handoff.AbortCapture);

    internal static IReadOnlyList<ZLinkActorHandoffFrame> EndDeferredJoinCapture(
        this ZLinkActorHandoffState handoff
    ) => Collect(handoff, handoff.EndDeferredJoinCapture);

    private static IReadOnlyList<ZLinkActorHandoffFrame> Collect(
        ZLinkActorHandoffState handoff,
        Action<Func<ZLinkActorHandoffFrame, Task, Task>> restore
    )
    {
        var frames = new List<ZLinkActorHandoffFrame>();
        restore(
            (frame, _) =>
            {
                frame.Withdraw = static () => ValueTask.FromResult(false);
                frames.Add(frame);
                return Task.CompletedTask;
            }
        );
        foreach (var frame in frames)
            handoff.ClaimDurableFrame(frame);
        return frames;
    }
}
