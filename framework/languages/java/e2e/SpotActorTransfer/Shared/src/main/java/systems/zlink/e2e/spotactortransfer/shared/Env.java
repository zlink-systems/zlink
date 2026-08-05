package systems.zlink.e2e.spotactortransfer.shared;

import java.io.Reader;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Properties;

public final class Env {
    private static final Properties VALUES = new Properties();

    private Env() {
    }

    public static synchronized void configure(String... args) {
        VALUES.clear();
        String path = configPath(args);
        if (path == null) {
            return;
        }
        try (Reader reader = Files.newBufferedReader(Path.of(path))) {
            VALUES.load(reader);
        } catch (Exception error) {
            throw new IllegalStateException("Could not load SpotActorTransfer E2E config", error);
        }
    }

    public static String get(String name, String fallback) {
        String value = VALUES.getProperty(name);
        return value == null || value.isBlank() ? fallback : value;
    }

    public static String require(String name) {
        String value = get(name, "");
        if (value.isBlank()) {
            throw new IllegalStateException(name + " is required");
        }
        return value;
    }

    private static String configPath(String... args) {
        for (int index = 0; index < args.length; index++) {
            if ("--e2e-config".equals(args[index]) && index + 1 < args.length) {
                return args[index + 1];
            }
            if (args[index].startsWith("--e2e-config=")) {
                return args[index].substring(args[index].indexOf('=') + 1);
            }
        }
        return null;
    }
}
