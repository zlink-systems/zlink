package systems.zlink.e2e.kotlin.toactormessaging.shared;

import java.io.Reader;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Properties;

public final class Env {
    private static Properties values;

    private Env() {
    }

    public static void configure(String[] args) {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank()) {
            throw new IllegalArgumentException("Usage: <role> --config <path>");
        }
        Properties loaded = new Properties();
        try (Reader reader = Files.newBufferedReader(Path.of(args[1]))) {
            loaded.load(reader);
        } catch (Exception error) {
            throw new IllegalStateException("Could not load Kotlin ToActorMessaging config.", error);
        }
        values = loaded;
    }

    public static String get(String key) {
        String value = properties().getProperty(key);
        if (value == null || value.isBlank()) {
            throw new IllegalStateException("Missing configuration value " + key);
        }
        return value;
    }

    public static String get(String key, String fallback) {
        String value = properties().getProperty(key);
        return value == null || value.isBlank() ? fallback : value;
    }

    private static Properties properties() {
        if (values == null) {
            throw new IllegalStateException("Kotlin ToActorMessaging configuration was not loaded.");
        }
        return values;
    }
}
