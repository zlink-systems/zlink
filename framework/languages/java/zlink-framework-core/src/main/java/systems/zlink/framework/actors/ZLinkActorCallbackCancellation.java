package systems.zlink.framework.actors;

/** Reports cancellation of an Actor binding callback. */
@FunctionalInterface
public interface ZLinkActorCallbackCancellation {
    boolean isCancellationRequested();
}
