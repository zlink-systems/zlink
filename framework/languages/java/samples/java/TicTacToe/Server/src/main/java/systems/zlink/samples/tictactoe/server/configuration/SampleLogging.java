package systems.zlink.samples.tictactoe.server.configuration;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;

public final class SampleLogging {
    private SampleLogging() {
    }

    public static void configure(SampleLogSettings settings, String role) {
        try {
            Files.createDirectories(Path.of(settings.logDirectory()));
            Path.of(settings.logDirectory(), role + ".log").toFile().createNewFile();
            Files.createDirectories(Path.of(settings.logDirectory()));
        } catch (IOException ex) {
            throw new IllegalStateException("Failed to initialize sample log files.", ex);
        }
    }

    public static String flowLogPath(SampleLogSettings settings, String role) {
        return Path.of(settings.logDirectory(), "flow-" + role + ".log").toString();
    }
}
