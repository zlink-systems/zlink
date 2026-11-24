package MyGame.e2e;

import MyGame.*;

import com.fasterxml.jackson.databind.JsonNode;
import com.sun.net.httpserver.HttpServer;

import java.io.*;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.time.Instant;
import java.time.ZoneOffset;
import java.time.format.DateTimeFormatter;
import java.util.*;
import java.util.concurrent.*;

public final class ServerHost implements ServerControl, AutoCloseable {
    static final String LOOPBACK = "127.0.0.1";
    private static final DateTimeFormatter CLOCK =
            DateTimeFormatter.ofPattern("HH:mm:ss.SSS").withZone(ZoneOffset.UTC);

    private final boolean console;
    private HttpServer controlServer;
    private final Path root;
    private final String runId = UUID.randomUUID().toString();
    final Path logs;
    private final String project = "my-game-" + runId;
    private final Map<String, Node> nodes = new LinkedHashMap<>();
    private final Map<String, Path> configs = new HashMap<>();
    private final Map<String, String> modules = new HashMap<>();
    private static final String DOCKER_PROJECT = System.getenv("MY_GAME_DOCKER_PROJECT");
    private static final boolean DOCKER = DOCKER_PROJECT != null;
    private boolean composeStarted;

    static boolean windows() {
        return System.getProperty("os.name").startsWith("Windows");
    }

    private final int tableCapacity;

    ServerHost(Path root, int tableCapacity, boolean console) throws IOException {
        this.console = console;
        this.tableCapacity = tableCapacity;
        this.root = root;
        logs = Files.createDirectories(root.resolve(".logs/run-" + runId));
    }

    public static void main(String[] args) throws Exception {
        var root = Path.of("").toAbsolutePath().normalize();
        if (Arrays.equals(args, new String[] {"--is-running"})) {
            System.exit(isRunning(root) ? 0 : 1);
        }
        Structure.require(args.length == 0, "Usage: run-server");
        Structure.check(root);
        var state = ServerControl.statePath(root);
        Files.createDirectories(state.getParent());
        try (var channel = openLock(state);
                var lock = channel.tryLock()) {
            Structure.require(
                    lock != null, "A server is already starting or running. Stop it with Ctrl+C.");
            Files.deleteIfExists(state);
            try (var host = new ServerHost(root, 0, true)) {
                Runtime.getRuntime()
                        .addShutdownHook(
                                new Thread(
                                        () -> {
                                            try {
                                                host.close();
                                            } catch (Exception failure) {
                                                failure.printStackTrace(System.err);
                                            }
                                        },
                                        "server-shutdown"));
                host.publish();
                new CountDownLatch(1).await();
            }
        }
    }

    private static FileChannel openLock(Path state) throws IOException {
        return FileChannel.open(
                state.resolveSibling("server.lock"),
                StandardOpenOption.CREATE,
                StandardOpenOption.WRITE);
    }

    static boolean isRunning(Path root) throws IOException {
        var state = ServerControl.statePath(root);
        if (!Files.isRegularFile(state)) return false;
        try (var channel = openLock(state);
                var lock = channel.tryLock()) {
            return lock == null && Files.isRegularFile(state);
        }
    }

    private synchronized void publish() throws Exception {
        var server = start();
        controlServer = ServerControl.publish(root, this, server.endpoint());
        announce("host", "Server ready: " + server.endpoint() + " (game clients connect here)");
        announce("host", "Logs: " + logs);
    }

    private void announce(String title, String message) {
        if (console)
            System.out.printf(
                    "%s  %-17s  %s%n", CLOCK.format(Instant.now()), "[" + title + "]", message);
    }

    private String compose(String... args) throws Exception {
        var command =
                new ArrayList<>(
                        List.of(
                                "docker",
                                "compose",
                                "-p",
                                project,
                                "-f",
                                root.resolve("docker-compose.yml").toString()));
        command.addAll(List.of(args));
        var process =
                new ProcessBuilder(command)
                        .directory(root.toFile())
                        .redirectErrorStream(true)
                        .start();
        String output =
                process.inputReader().lines().collect(java.util.stream.Collectors.joining("\n"));
        Files.writeString(
                logs.resolve("compose.log"),
                output,
                StandardOpenOption.CREATE,
                StandardOpenOption.APPEND);
        Structure.require(process.waitFor() == 0, "Compose failed: " + output);
        return output.trim();
    }

    ServerControl.Connection start() throws Exception {
        String endpoint = null;
        if (!DOCKER) {
            composeStarted = true;
            compose("up", "-d", "--wait", "redis");
        }
        String redis = (DOCKER ? "redis:6379" : compose("port", "redis", "6379"));
        announce("host", "Redis started — listen " + redis);
        var structure = Json.MAPPER.readTree(root.resolve("zlink.structure.json").toFile());
        var declarations = new ArrayList<JsonNode>();
        structure.path("nodes").forEach(declarations::add);
        declarations.sort(Comparator.comparing(n -> n.path("role").asText().equals("session")));
        for (var declaration : declarations) {
            String role = declaration.path("role").asText();
            if (!Set.of("session", "table", "matchmaker").contains(role)) continue;
            var declared =
                    Json.MAPPER.readValue(
                            root.resolve(declaration.path("config").asText()).toFile(),
                            NodeConfig.class);
            String id = declared.id();
            modules.put(id, role);
            var config =
                    new NodeConfig(
                            id,
                            redis,
                            "my-game:" + runId + ":",
                            "tcp://" + LOOPBACK + ":0",
                            role.equals("session") ? "ws://" + LOOPBACK + ":0" : null,
                            tableCapacity);
            var path = logs.resolve(id + ".json");
            Json.MAPPER.writeValue(path.toFile(), config);
            configs.put(id, path);
            var ready = startNode(id, declaration.path("title").asText());
            if (role.equals("session")) endpoint = ready.path("streamEndpoint").asText();
        }
        Structure.require(
                endpoint != null && !endpoint.isBlank(), "Session STREAM endpoint missing");
        return new ServerControl.Connection(endpoint, this);
    }

    private JsonNode startNode(String id, String title) throws Exception {
        var generationLogs = Files.createDirectories(logs.resolve(id + "-" + UUID.randomUUID()));
        var node = new Node(id, title, modules.get(id), configs.get(id), generationLogs);
        nodes.put(id, node);
        return Json.MAPPER.readTree(node.waitFor(Control.READY).substring(Control.READY.length()));
    }

    public synchronized void drain(String id) throws Exception {
        var node = Objects.requireNonNull(nodes.get(id), "Unknown node " + id);
        node.command(Control.DRAIN);
        for (var stage :
                List.of(Control.RELOCATION_REQUESTED, Control.RELOCATED, Control.SAFE_TO_SHUTDOWN))
            node.waitFor(stage);
    }

    public synchronized void replace(String id) throws Exception {
        var node = Objects.requireNonNull(nodes.remove(id), "Unknown node " + id);
        node.cleanup();
        startNode(id, node.title);
    }

    public synchronized void close() throws Exception {
        Exception failure = null;
        if (controlServer != null) {
            controlServer.stop(0);
            controlServer = null;
            try {
                Files.deleteIfExists(ServerControl.statePath(root));
            } catch (IOException error) {
                failure = error;
            }
        }
        if (!nodes.isEmpty()) announce("host", "Stopping servers");
        for (var node : new ArrayList<>(nodes.values()).reversed())
            try {
                node.cleanup();
            } catch (Exception error) {
                if (failure == null) failure = error;
                else failure.addSuppressed(error);
            }
        nodes.clear();
        if (composeStarted) {
            composeStarted = false;
            try {
                compose("down", "--volumes", "--remove-orphans");
                announce("host", "Redis stopped");
            } catch (Exception error) {
                if (failure == null) failure = error;
                else failure.addSuppressed(error);
            }
        }
        if (failure != null) throw failure;
    }

    private static void docker(String... arguments) throws Exception {
        var command = new ArrayList<>(List.of("docker"));
        command.addAll(List.of(arguments));
        var process = new ProcessBuilder(command).redirectErrorStream(true).start();
        String output =
                process.inputReader().lines().collect(java.util.stream.Collectors.joining("\n"));
        Structure.require(process.waitFor() == 0, "Docker failed: " + output);
    }

    private final class Node {
        private static final long DOCKER_CLEANUP_SECONDS = 120;

        private final String id;
        private final String title;
        private final Path logs;
        private final Process process;
        private final String container;
        private final BufferedWriter input;
        private final LinkedBlockingQueue<String> output = new LinkedBlockingQueue<>();

        Node(String id, String title, String module, Path config, Path logs) throws Exception {
            this.id = id;
            this.title = title;
            this.logs = logs;
            List<String> command;
            container =
                    DOCKER
                            ? DOCKER_PROJECT
                                    + "-"
                                    + id.toLowerCase(Locale.ROOT)
                                    + "-"
                                    + UUID.randomUUID()
                            : null;
            if (DOCKER) {
                docker(
                        "create",
                        "-i",
                        "--name",
                        container,
                        "--label",
                        "my-game.e2e=" + DOCKER_PROJECT,
                        "--network",
                        "container:" + System.getenv("MY_GAME_RUNNER_CONTAINER"),
                        "--env",
                        "MYGAME_LOG_DIR=/app/logs",
                        System.getenv("MY_GAME_NODE_IMAGE"),
                        "-Dstdout.encoding=UTF-8",
                        "-Dstderr.encoding=UTF-8",
                        "-cp",
                        "/app/" + module + "/lib/*",
                        "MyGame." + module + ".Program",
                        "/app/config.json");
                docker("cp", config.toString(), container + ":/app/config.json");
                command = new ArrayList<>(List.of("docker", "start", "-a", "-i", container));
            } else {
                command =
                        new ArrayList<>(
                                List.of(
                                        Path.of(
                                                        System.getProperty("java.home"),
                                                        "bin",
                                                        windows() ? "java.exe" : "java")
                                                .toString(),
                                        "--enable-native-access=ALL-UNNAMED",
                                        "-Dstdout.encoding=UTF-8",
                                        "-Dstderr.encoding=UTF-8",
                                        "-cp",
                                        root.resolve(
                                                                "server/"
                                                                        + module
                                                                        + "/build/install/"
                                                                        + module
                                                                        + "/lib")
                                                        .toString()
                                                + "/*",
                                        "MyGame." + module + ".Program",
                                        config.toString()));
                if (console && !windows()) {
                    // The host handles terminal interrupts and sends shutdown over stdin.
                    command.addAll(0, List.of("sh", "-c", "trap '' INT; exec \"$@\"", "node"));
                }
            }
            var builder =
                    new ProcessBuilder(command)
                            .directory(root.toFile())
                            .redirectError(logs.resolve(id + "-stderr.log").toFile());
            if (!DOCKER) builder.environment().put("MYGAME_LOG_DIR", logs.toString());
            process = builder.start();
            input = process.outputWriter();
            Thread.ofPlatform()
                    .daemon()
                    .name(id + "-output")
                    .start(
                            () -> {
                                try (var writer =
                                                Files.newBufferedWriter(
                                                        logs.resolve(id + "-stdout.log"));
                                        var reader = process.inputReader(StandardCharsets.UTF_8)) {
                                    for (String line; (line = reader.readLine()) != null; ) {
                                        writer.write(line);
                                        writer.newLine();
                                        writer.flush();
                                        if (isControlLine(line)) output.add(line);
                                        else announce(title, line);
                                    }
                                    output.add(Control.PROCESS_EXIT + process.waitFor());
                                } catch (Exception error) {
                                    output.add(Control.PROCESS_EXIT + error);
                                }
                            });
        }

        private boolean isControlLine(String line) {
            return line.startsWith(Control.READY)
                    || line.startsWith(Control.RELOCATION_REQUESTED)
                    || line.startsWith(Control.RELOCATED)
                    || line.startsWith(Control.SAFE_TO_SHUTDOWN)
                    || line.startsWith(Control.STOPPED);
        }

        String waitFor(String prefix) throws Exception {
            long expires =
                    System.nanoTime()
                            + (prefix.equals(Control.SAFE_TO_SHUTDOWN)
                                    ? Control.SAFE_DEADLINE.toNanos()
                                    : TimeUnit.SECONDS.toNanos(Control.PROCESS_SECONDS));
            while (true) {
                var line =
                        output.poll(Math.max(0, expires - System.nanoTime()), TimeUnit.NANOSECONDS);
                if (line == null)
                    throw new IllegalStateException(
                            id + " timed out waiting for " + prefix + "; inspect node stderr");
                if (line.startsWith(Control.PROCESS_EXIT))
                    throw new IllegalStateException(id + " exited before " + prefix + ": " + line);
                if (line.startsWith(prefix)) return line;
            }
        }

        void command(String command) throws Exception {
            input.write(command);
            input.newLine();
            input.flush();
        }

        void stop() throws Exception {
            Structure.require(process.isAlive(), id + " exited unexpectedly");
            command(Control.SHUTDOWN);
            String stopped = waitFor(Control.STOPPED);
            if (container != null) {
                Structure.require(
                        stopped.equals(Control.STOPPED), id + " shutdown failed: " + stopped);
                if (!process.waitFor(DOCKER_CLEANUP_SECONDS, TimeUnit.SECONDS)) {
                    System.err.println(
                            "infra cleanup delay: " + id + " " + DOCKER_CLEANUP_SECONDS + "s");
                    process.destroyForcibly().waitFor();
                    return;
                }
            } else if (!process.waitFor(Control.PROCESS_SECONDS, TimeUnit.SECONDS)) {
                dumpThreads();
                throw new IllegalStateException(
                        id + " failed to exit; thread dump is in " + id + "-stdout.log");
            }
            Structure.require(
                    process.exitValue() == 0, id + " shutdown exit: " + process.exitValue());
        }

        // SIGQUIT makes HotSpot print a thread dump to stdout.
        private void dumpThreads() throws InterruptedException {
            try {
                if (!windows())
                    new ProcessBuilder("kill", "-3", Long.toString(process.pid()))
                            .start()
                            .waitFor(Control.THREAD_DUMP_SECONDS, TimeUnit.SECONDS);
                else return;
            } catch (Exception ignored) {
                return;
            }
            Thread.sleep(TimeUnit.SECONDS.toMillis(Control.THREAD_DUMP_SECONDS));
        }

        void cleanup() throws Exception {
            Throwable primary = null;
            try {
                stop();
            } catch (Exception | Error error) {
                primary = error;
                throw error;
            } finally {
                try {
                    if (process.isAlive()) {
                        process.destroyForcibly();
                        process.waitFor();
                    }
                    if (container != null) {
                        try {
                            docker(
                                    "cp",
                                    container + ":/app/logs/" + id + ".flow.log",
                                    logs.toString());
                        } finally {
                            docker("rm", "-f", container);
                        }
                    }
                } catch (Exception | Error error) {
                    if (primary != null) primary.addSuppressed(error);
                    else throw error;
                }
            }
        }
    }
}
