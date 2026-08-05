package systems.zlink.e2e.storefailure.shared;

import java.time.Duration;
import java.time.Instant;
import java.util.concurrent.Callable;

public final class Wait {
    private Wait() {
    }

    public static <T> T until(
        Duration timeout,
        String failureMessage,
        Callable<T> attempt) {
        Instant deadline = Instant.now().plus(timeout);
        Exception last = null;
        while (Instant.now().isBefore(deadline)) {
            try {
                T result = attempt.call();
                if (result != null) {
                    return result;
                }
            } catch (Exception error) {
                last = error;
            }
            sleep(Duration.ofMillis(100));
        }
        throw new IllegalStateException(failureMessage, last);
    }

    public static void sleep(Duration duration) {
        try {
            Thread.sleep(duration.toMillis());
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("interrupted while waiting", error);
        }
    }
}
