package MyGame.e2e;

import MyGame.Json;

import com.fasterxml.jackson.databind.JsonNode;

import java.nio.channels.FileChannel;
import java.nio.file.*;
import java.util.Arrays;
import java.util.concurrent.CountDownLatch;

public final class ServerHost {
    private ServerHost() {}

    public static void main(String[] args) throws Exception {
        var root = Path.of("").toAbsolutePath().normalize();
        if (Arrays.equals(args, new String[] {"--is-running"})) {
            System.exit(connection(root) == null ? 1 : 0);
        }
        Structure.require(args.length == 0, "Usage: run-server");
        var state = statePath(root);
        Files.createDirectories(state.getParent());
        try (var channel =
                        FileChannel.open(
                                state.resolveSibling("server.lock"),
                                StandardOpenOption.CREATE,
                                StandardOpenOption.WRITE);
                var lock = channel.tryLock()) {
            Structure.require(
                    lock != null, "A server is already starting or running. Stop it with Ctrl+C.");
            Files.deleteIfExists(state);
            Structure.check(root);
            var nodes = new Nodes(root);
            nodes.console = true;
            var shutdown =
                    new Thread(
                            () -> {
                                synchronized (nodes) {
                                    try {
                                        Files.deleteIfExists(state);
                                    } catch (Exception failure) {
                                        failure.printStackTrace(System.err);
                                    } finally {
                                        try {
                                            nodes.close();
                                        } catch (Exception failure) {
                                            failure.printStackTrace(System.err);
                                        }
                                    }
                                }
                            },
                            "server-shutdown");
            Runtime.getRuntime().addShutdownHook(shutdown);
            System.out.println("Logs: " + nodes.logs);
            try (nodes) {
                synchronized (nodes) {
                    nodes.start();
                    var connection = nodes.connection();
                    connection.put("pid", ProcessHandle.current().pid());
                    connection.put(
                            "started",
                            ProcessHandle.current().info().startInstant().orElseThrow().toString());
                    var temporary = Files.createTempFile(state.getParent(), "server-", ".tmp");
                    try {
                        Json.MAPPER.writeValue(temporary.toFile(), connection);
                        Files.move(temporary, state, StandardCopyOption.ATOMIC_MOVE);
                    } finally {
                        Files.deleteIfExists(temporary);
                    }
                    nodes.printReady();
                }
                new CountDownLatch(1).await();
            } catch (Exception failure) {
                RunLogs.failure(nodes.logs, "server", failure);
                throw failure;
            } finally {
                Files.deleteIfExists(state);
                Runtime.getRuntime().removeShutdownHook(shutdown);
            }
        }
    }

    private static Path statePath(Path root) {
        return root.resolve(".logs/server.json");
    }

    static JsonNode connection(Path root) throws Exception {
        JsonNode state;
        try (var reader = Files.newInputStream(statePath(root))) {
            state = Json.MAPPER.readTree(reader);
        } catch (NoSuchFileException missing) {
            return null;
        }
        return ProcessHandle.of(state.path("pid").asLong())
                        .flatMap(process -> process.info().startInstant())
                        .filter(start -> start.toString().equals(state.path("started").asText()))
                        .isPresent()
                ? state
                : null;
    }
}
