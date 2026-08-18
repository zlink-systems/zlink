package systems.zlink.crosslanguage.host;

import java.io.IOException;
import java.io.UncheckedIOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;

/** Appends markers to stdout and, if configured, to an event file — the same
 * convention the C++/.NET/Node cross-language peer hosts use so the shell
 * runner's {@code wait_for_line} can assert on them. */
public final class EventSink {
    private final Path eventFile;
    private final Object lock = new Object();

    public EventSink(String eventFilePath) {
        this.eventFile = eventFilePath == null || eventFilePath.isBlank()
            ? null
            : Path.of(eventFilePath);
    }

    public void append(String line) {
        System.out.println(line);
        if (eventFile == null) {
            return;
        }
        synchronized (lock) {
            try {
                Files.writeString(
                    eventFile,
                    line + System.lineSeparator(),
                    StandardCharsets.UTF_8,
                    StandardOpenOption.CREATE, StandardOpenOption.APPEND);
            } catch (IOException error) {
                throw new UncheckedIOException(error);
            }
        }
    }
}
