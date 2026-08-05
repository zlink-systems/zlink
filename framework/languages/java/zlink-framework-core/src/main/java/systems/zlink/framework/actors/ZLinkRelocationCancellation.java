package systems.zlink.framework.actors;

/**
 * Reports whether the current relocation attempt may still publish a result.
 */
public interface ZLinkRelocationCancellation {
    boolean isCancellationRequested();
}
