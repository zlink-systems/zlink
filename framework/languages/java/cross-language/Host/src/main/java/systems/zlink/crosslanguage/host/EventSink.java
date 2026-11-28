package systems.zlink.crosslanguage.host;

import java.io.IOException;
import java.io.UncheckedIOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Appends markers to stdout and, if configured, to an event file — the same convention the
 * C++/.NET/Node cross-language peer hosts use so the shell runner's {@code wait_for_line} can
 * assert on them.
 */
public final class EventSink implements AutoCloseable {
    private final Path eventFile;
    private final ExecutorService writer =
            Executors.newSingleThreadExecutor(
                    task -> new Thread(task, "cross-language-event-writer"));
    private final AtomicReference<UncheckedIOException> writeFailure = new AtomicReference<>();

    public EventSink(String eventFilePath) {
        this.eventFile =
                eventFilePath == null || eventFilePath.isBlank() ? null : Path.of(eventFilePath);
    }

    public void append(String line) {
        writer.execute(() -> write(line));
    }

    private void write(String line) {
        System.out.println(line);
        if (eventFile == null) {
            return;
        }
        try {
            Files.writeString(
                    eventFile,
                    line + System.lineSeparator(),
                    StandardCharsets.UTF_8,
                    StandardOpenOption.CREATE,
                    StandardOpenOption.APPEND);
        } catch (IOException error) {
            writeFailure.compareAndSet(null, new UncheckedIOException(error));
        }
    }

    @Override
    public void close() {
        writer.close();
        var failure = writeFailure.get();
        if (failure != null) {
            throw failure;
        }
    }
}
