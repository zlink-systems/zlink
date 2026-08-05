package systems.zlink.framework.runtime.configuration;

import java.time.Duration;
import systems.zlink.framework.configuration.ZLinkWorkerOptions;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.execution.ZLinkWorkerPool;

public final class ZLinkWorkerOptionsRegistration implements ZLinkWorkerOptions {
    private int minThreads;
    private int maxThreads = ZLinkWorkerPool.defaultMaxThreads();
    private Duration idleTimeout = Duration.ofSeconds(30);
    private int maxQueueLength = 1024;

    @Override
    public ZLinkWorkerOptionsRegistration minThreads(int minThreads) {
        if (minThreads < 0) {
            throw new ZLinkConfigurationException("worker minThreads must be >= 0");
        }
        this.minThreads = minThreads;
        return this;
    }

    @Override
    public ZLinkWorkerOptionsRegistration maxThreads(int maxThreads) {
        if (maxThreads < 1) {
            throw new ZLinkConfigurationException("worker maxThreads must be >= 1");
        }
        this.maxThreads = maxThreads;
        return this;
    }

    @Override
    public ZLinkWorkerOptionsRegistration idleTimeout(Duration idleTimeout) {
        if (idleTimeout == null || idleTimeout.isNegative()) {
            throw new ZLinkConfigurationException("worker idleTimeout must not be negative");
        }
        this.idleTimeout = idleTimeout;
        return this;
    }

    @Override
    public ZLinkWorkerOptionsRegistration maxQueueLength(int maxQueueLength) {
        if (maxQueueLength < 1) {
            throw new ZLinkConfigurationException("worker maxQueueLength must be >= 1");
        }
        this.maxQueueLength = maxQueueLength;
        return this;
    }

    public int minThreads() {
        return minThreads;
    }

    public int maxThreads() {
        return maxThreads;
    }

    public Duration idleTimeout() {
        return idleTimeout;
    }

    public int maxQueueLength() {
        return maxQueueLength;
    }

    public ZLinkWorkerPool createPool() {
        return new ZLinkWorkerPool(minThreads, maxThreads, idleTimeout, maxQueueLength);
    }

    void validate() {
        if (maxThreads < minThreads) {
            throw new ZLinkConfigurationException(
                "worker maxThreads must be >= minThreads");
        }
    }
}
