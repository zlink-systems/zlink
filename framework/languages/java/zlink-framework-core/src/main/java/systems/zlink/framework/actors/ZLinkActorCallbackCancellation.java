package systems.zlink.framework.actors;

/** Reports whether a duplicate-bind callback exceeded its configured deadline. */
@FunctionalInterface
public interface ZLinkActorCallbackCancellation {
    boolean isCancellationRequested();
}
