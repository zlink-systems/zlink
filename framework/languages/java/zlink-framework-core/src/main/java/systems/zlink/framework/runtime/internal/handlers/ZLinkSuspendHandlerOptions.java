package systems.zlink.framework.runtime.internal.handlers;

/** Internal bridge used by the official Kotlin extension package. */
public interface ZLinkSuspendHandlerOptions {
    void useSuspendHandlerInvoker(ZLinkSuspendInvocationAdapter invoker);
}
