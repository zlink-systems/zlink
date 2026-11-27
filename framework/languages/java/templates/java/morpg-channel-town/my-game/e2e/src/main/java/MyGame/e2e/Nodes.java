package MyGame.e2e;

import MyGame.*;

import com.fasterxml.jackson.databind.JsonNode;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;

public final class Nodes implements AutoCloseable {
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
    boolean console;
    String endpoint;

    static boolean windows() {
        return System.getProperty("os.name").startsWith("Windows");
    }

    Nodes(Path root) throws Exception {
        this(root, root.resolve(".logs/run-" + UUID.randomUUID()));
    }

    Nodes(Path root, Path logs) throws Exception {
        this.root = root;
        this.logs = Files.createDirectories(logs);
    }

    com.fasterxml.jackson.databind.node.ObjectNode connection() {
        var state = Json.MAPPER.createObjectNode();
        state.put("endpoint", endpoint);
        return state;
    }

    void attach(JsonNode state) {
        endpoint = state.path("endpoint").asText();
    }

    void printReady() {
        System.out.println("Server ready: " + endpoint + " (game clients connect here)");
        System.out.println("Logs: " + logs);
    }

    private void announce(String node, String message) {
        if (console) System.out.printf("[%s] %s%n", node, message);
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

    synchronized void start() throws Exception {
        if (!DOCKER) {
            composeStarted = true;
            announce("Redis", "Starting");
            compose("up", "-d", "--wait", "redis");
        }
        String redis = (DOCKER ? "redis:6379" : compose("port", "redis", "6379"));
        announce("Redis", "Ready");
        var structure = Json.MAPPER.readTree(root.resolve("zlink.structure.json").toFile());
        var declarations = new ArrayList<JsonNode>();
        structure.path("nodes").forEach(declarations::add);
        declarations.sort(
                Comparator.comparing(n -> n.path("role").asText().equals(NodeRole.SESSION.id())));
        for (var declaration : declarations) {
            var role = NodeRole.parse(declaration.path("role").asText());
            if (!role.application()) continue;
            var declared =
                    Json.MAPPER.readValue(
                            root.resolve(declaration.path("config").asText()).toFile(),
                            NodeConfig.class);
            String id = declared.id();
            modules.put(id, role.id());
            var config =
                    new NodeConfig(
                            id,
                            redis,
                            "my-game:" + runId + ":",
                            "tcp://127.0.0.1:0",
                            role == NodeRole.SESSION ? "ws://127.0.0.1:0" : null,
                            declared.townCapacity(),
                            console && declared.dungeonCapacity() > 0
                                    ? Integer.MAX_VALUE
                                    : declared.dungeonCapacity(),
                            declared.townIds());
            var path = logs.resolve(id + ".json");
            Json.MAPPER.writeValue(path.toFile(), config);
            configs.put(id, path);
            var ready = startNode(id);
            if (role == NodeRole.SESSION) endpoint = ready.path("streamEndpoint").asText();
        }
    }

    private JsonNode startNode(String id) throws Exception {
        var generationLogs = Files.createDirectories(logs.resolve(id + "-" + UUID.randomUUID()));
        var node = new Node(id, modules.get(id), configs.get(id), root, generationLogs);
        nodes.put(id, node);
        return Json.MAPPER.readTree(node.waitFor(Control.READY).substring(Control.READY.length()));
    }

    void drain(String id) throws Exception {
        var node = Objects.requireNonNull(nodes.get(id));
        node.command(Control.DRAIN);
        for (var stage :
                List.of(Control.RELOCATION_REQUESTED, Control.RELOCATED, Control.SAFE_TO_SHUTDOWN))
            node.waitFor(stage);
    }

    void replace(String id) throws Exception {
        nodes.remove(id).cleanup();
        startNode(id);
    }

    public synchronized void close() throws Exception {
        Exception failure = null;
        for (var node : new ArrayList<>(nodes.values()).reversed())
            try {
                node.cleanup();
            } catch (Exception error) {
                if (failure == null) failure = error;
                else failure.addSuppressed(error);
            }
        nodes.clear();
        if (composeStarted)
            try {
                composeStarted = false;
                try {
                    Files.writeString(logs.resolve("redis.log"), compose("logs", "--no-color"));
                } finally {
                    compose("down", "--volumes", "--remove-orphans");
                    announce("Redis", "Stopped");
                }
            } catch (Exception error) {
                if (failure == null) failure = error;
                else failure.addSuppressed(error);
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
        private final Path logs;
        private final Process process;
        private final String container;
        private final BufferedWriter input;
        private final LinkedBlockingQueue<String> output = new LinkedBlockingQueue<>();

        Node(String id, String module, Path config, Path root, Path logs) throws Exception {
            this.id = id;
            this.logs = logs;
            announce(id, "Starting");
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
                        System.getenv("MY_GAME_NODE_IMAGE"),
                        "-DMyGame.flowFile=/app/logs/" + id + ".flow.log",
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
                                        "-DMyGame.flowFile=" + logs.resolve(id + ".flow.log"),
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
            }
            if (console) {
                // The host handles Ctrl+C and shuts down nodes through their stdin.
                if (windows()) command.add(1, "-Xrs");
                else command.addAll(0, List.of("sh", "-c", "trap '' INT; exec \"$@\"", "node"));
            }
            process =
                    new ProcessBuilder(command)
                            .directory(root.toFile())
                            .redirectError(logs.resolve(id + "-stderr.log").toFile())
                            .start();
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
                                        if (line.startsWith(Control.READY)) announce(id, "Ready");
                                        if (line.startsWith(Control.READY)
                                                || line.startsWith(Control.RELOCATION_REQUESTED)
                                                || line.startsWith(Control.RELOCATED)
                                                || line.startsWith(Control.SAFE_TO_SHUTDOWN)
                                                || line.startsWith(Control.STOPPED))
                                            output.add(line);
                                        else announce(id, line);
                                    }
                                    output.add(Control.PROCESS_EXIT + process.waitFor());
                                } catch (Exception error) {
                                    output.add(Control.PROCESS_EXIT + error);
                                }
                            });
        }

        String waitFor(String prefix) throws Exception {
            long expires =
                    System.nanoTime()
                            + TimeUnit.SECONDS.toNanos(
                                    prefix.equals(Control.SAFE_TO_SHUTDOWN)
                                            ? Control.SAFE_DEADLINE.toSeconds()
                                            : Control.PROCESS_SECONDS);
            while (true) {
                var line =
                        output.poll(Math.max(0, expires - System.nanoTime()), TimeUnit.NANOSECONDS);
                if (line == null)
                    throw new IllegalStateException(
                            id
                                    + " timed out waiting for "
                                    + prefix
                                    + "; inspect "
                                    + logs.resolve(id + "-stderr.log"));
                if (line.startsWith(Control.PROCESS_EXIT))
                    throw new IllegalStateException(
                            id
                                    + " exited before "
                                    + prefix
                                    + ": "
                                    + line
                                    + "; inspect "
                                    + logs.resolve(id + "-stderr.log"));
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
                    announce(id, "Stopped");
                } catch (Exception | Error error) {
                    if (primary != null) primary.addSuppressed(error);
                    else throw error;
                }
            }
        }
    }
}
