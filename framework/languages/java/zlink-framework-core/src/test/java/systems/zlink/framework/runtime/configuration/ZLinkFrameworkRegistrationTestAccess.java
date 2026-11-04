package systems.zlink.framework.runtime.configuration;

import java.util.Objects;
import java.util.concurrent.ExecutorService;

public final class ZLinkFrameworkRegistrationTestAccess {
    private ZLinkFrameworkRegistrationTestAccess() {}

    public static DefaultZLinkFrameworkOptions optionsWithSerialExecutor(ExecutorService executor) {
        return new DefaultZLinkFrameworkOptions(
                new ZLinkFrameworkRegistration(Objects.requireNonNull(executor, "executor")));
    }
}
