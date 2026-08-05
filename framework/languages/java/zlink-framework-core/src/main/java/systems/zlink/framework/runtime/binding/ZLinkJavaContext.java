package systems.zlink.framework.runtime.binding;

import systems.zlink.contracts.core.Context;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;

record ZLinkJavaContext(Context nativeContext) implements ZLinkBackendContext {
    @Override public String name() { return "context"; }
    @Override public void shutdown() { nativeContext.shutdown(); }

    @Override
    public void close() {
        try {
            nativeContext.shutdown();
        } finally {
            nativeContext.close();
        }
    }
}
