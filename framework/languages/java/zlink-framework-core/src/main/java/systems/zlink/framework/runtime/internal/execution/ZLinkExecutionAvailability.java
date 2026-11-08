package systems.zlink.framework.runtime.internal.execution;

/** Lets the existing execution queue retain accepted application work during confirmation. */
public interface ZLinkExecutionAvailability {
    boolean paused();

    void onResume(Runnable action);
}
