package systems.zlink.framework.runtime.configuration;

import java.util.concurrent.ExecutorService;

public final class ZLinkFrameworkRegistrationTestAccess {
    private ZLinkFrameworkRegistrationTestAccess() {}

    public static ExecutorService replaceSerialExecutor(
            ZLinkFrameworkRegistration registration, ExecutorService executor) {
        ExecutorService previous = registration.serialExecutor;
        registration.serialExecutor = executor;
        return previous;
    }
}
