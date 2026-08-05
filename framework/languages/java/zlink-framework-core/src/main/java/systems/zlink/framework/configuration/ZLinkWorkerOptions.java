package systems.zlink.framework.configuration;

import java.time.Duration;

/**
 * Configuration for the single elastic bounded worker pool backing
 * {@code context.runCpuWorker(...)}.
 *
 * <p>Defaults: {@code minThreads=0}, {@code maxThreads=max(2, cpuCount*2)},
 * {@code idleTimeout=30s}, {@code maxQueueLength=1024}. A full queue fails the
 * submit immediately; there is no wait or caller-runs policy.
 */
public interface ZLinkWorkerOptions {
    ZLinkWorkerOptions minThreads(int minThreads);

    ZLinkWorkerOptions maxThreads(int maxThreads);

    ZLinkWorkerOptions idleTimeout(Duration idleTimeout);

    ZLinkWorkerOptions maxQueueLength(int maxQueueLength);
}
