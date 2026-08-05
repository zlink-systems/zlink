package systems.zlink.framework.runtime.diagnostics;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.channels.FileLock;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;

// Append writer for the dedicated tracing file, mirroring the C++ file sink (open-
// append per line). Creates parent directories. Thread-safe within the JVM (monitor)
// and across processes/runtimes (exclusive FileLock on the append channel), so
// multiple nodes sharing one trace file never interleave partial lines.
final class ZLinkTraceFileWriter {
    private static final Object GATE = new Object();

    private ZLinkTraceFileWriter() {
    }

    static void write(String path, String line) {
        try {
            Path file = Path.of(path);
            byte[] bytes = (line + System.lineSeparator()).getBytes(StandardCharsets.UTF_8);
            synchronized (GATE) {
                Path parent = file.getParent();
                if (parent != null) {
                    Files.createDirectories(parent);
                }
                try (FileChannel channel = FileChannel.open(
                        file,
                        StandardOpenOption.CREATE,
                        StandardOpenOption.WRITE,
                        StandardOpenOption.APPEND);
                     FileLock lock = channel.lock()) {
                    channel.write(ByteBuffer.wrap(bytes));
                }
            }
        } catch (IOException ignored) {
            // best-effort: tracing must never break dispatch.
        }
    }
}
