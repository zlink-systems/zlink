package systems.zlink.e2e.storefailure.consumer;

import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;

final class LocationStoreDelayState {
    private final Path controlFile;
    private volatile int delayMilliseconds;

    LocationStoreDelayState(String controlFile) {
        this.controlFile = controlFile == null || controlFile.isBlank()
            ? null
            : Path.of(controlFile);
    }

    int delayMilliseconds() {
        return delayMilliseconds;
    }

    void setDelay(Duration delay) {
        long clamped = Math.max(0, Math.min(5000, delay.toMillis()));
        delayMilliseconds = (int) clamped;
        if (controlFile == null) {
            return;
        }
        try {
            Files.writeString(controlFile, Integer.toString(delayMilliseconds));
        } catch (java.io.IOException error) {
            throw new IllegalStateException("failed to update store delay control", error);
        }
    }
}
