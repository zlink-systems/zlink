package MyGame.e2e;

import java.io.IOException;
import java.io.PrintWriter;
import java.nio.file.*;
import java.util.UUID;

final class RunLogs {
    private RunLogs() {}

    static Path create(Path root) throws IOException {
        var logs = Files.createDirectories(root.resolve(".logs/run-" + UUID.randomUUID()));
        System.out.println("Logs: " + logs);
        return logs;
    }

    static void heading(String name, int index, int total) {
        System.out.printf("━━ %s (%d/%d) ━━%n", name, index, total);
    }

    static void failure(Path logs, String name, Exception error) throws IOException {
        var detail = logs.resolve(name + "-failure.log");
        try (var writer = new PrintWriter(Files.newBufferedWriter(detail))) {
            error.printStackTrace(writer);
        }
        var first = error.toString().lines().findFirst().orElse(error.getClass().getSimpleName());
        System.out.printf("✗ %s: %s (%s)%n", name, first, detail);
    }
}
