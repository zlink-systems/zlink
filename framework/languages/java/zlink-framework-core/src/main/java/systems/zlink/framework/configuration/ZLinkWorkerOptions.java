package systems.zlink.framework.configuration;

import java.time.Duration;

/**
 * Configuration for the single elastic bounded worker pool backing
 * {@code context.runCpuWorker(...)}.
 *
 * <p>Defaults: {@code minThreads=0}, {@code maxThreads=max(2, cpuCount*2)},
 * {@code idleTimeout=30s}. Work waits for a worker when every worker is busy.
 */
public interface ZLinkWorkerOptions {
    ZLinkWorkerOptions minThreads(int minThreads);

    ZLinkWorkerOptions maxThreads(int maxThreads);

    ZLinkWorkerOptions idleTimeout(Duration idleTimeout);

}
