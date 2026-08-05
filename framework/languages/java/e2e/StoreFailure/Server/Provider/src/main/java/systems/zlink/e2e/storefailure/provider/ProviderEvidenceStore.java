package systems.zlink.e2e.storefailure.provider;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import systems.zlink.e2e.storefailure.shared.Wait;

public final class ProviderEvidenceStore {
    private final ProviderOptions options;
    private final List<String> entries = new ArrayList<>();

    public ProviderEvidenceStore(ProviderOptions options) {
        this.options = options;
    }

    public String rid() {
        return options.rid();
    }

    public synchronized void record(String marker, String value) {
        String line = "rid=" + options.rid() + " marker=" + marker + " value=" + value;
        entries.add(line);
        if (options.evidenceFile() != null && !options.evidenceFile().isBlank()) {
            try {
                Path path = Path.of(options.evidenceFile());
                Files.createDirectories(path.getParent());
                Files.writeString(
                    path,
                    line + System.lineSeparator(),
                    StandardCharsets.UTF_8,
                    java.nio.file.StandardOpenOption.CREATE,
                    java.nio.file.StandardOpenOption.APPEND);
            } catch (IOException error) {
                throw new IllegalStateException("failed to write evidence", error);
            }
        }
    }

    public synchronized List<String> snapshot() {
        return List.copyOf(entries);
    }

    public List<String> waitFor(String contains, Duration timeout) {
        return Wait.until(timeout, "timed out waiting for provider evidence " + contains, () -> {
            List<String> snapshot = snapshot();
            return snapshot.stream().anyMatch(line -> line.contains(contains)) ? snapshot : null;
        });
    }
}
