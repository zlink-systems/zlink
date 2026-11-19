package systems.zlink.crosslanguage.host;

import java.io.IOException;
import java.io.UncheckedIOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.Properties;

/** One shared lease profile for all outage hosts. */
public final class OutageLeaseProfile {
    private static final Properties VALUES = load();
    private OutageLeaseProfile() {}
    private static Properties load() {
        String file = System.getenv("ZLINK_OUTAGE_LEASE_PROFILE");
        if (file == null || file.isBlank()) throw new IllegalStateException("ZLINK_OUTAGE_LEASE_PROFILE is required");
        Properties values = new Properties();
        try (var input = Files.newBufferedReader(Path.of(file))) { values.load(input); }
        catch (IOException error) { throw new UncheckedIOException(error); }
        return values;
    }
    public static Duration duration(String key) {
        return Duration.ofMillis(Long.parseLong(VALUES.getProperty(key)));
    }
    public static boolean retention() {
        if (!"true".equals(VALUES.getProperty("storeOutageRetention")))
            throw new IllegalStateException("outage profile requires StoreOutageRetention");
        return true;
    }
    public static long lifetimeNanos() {
        return duration("ttlMs").plus(duration("fencingMarginMs").multipliedBy(2)).toNanos();
    }
}
