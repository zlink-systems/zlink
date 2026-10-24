package MyGame.e2e;

import MyGame.*;

import com.fasterxml.jackson.databind.JsonNode;

import java.io.*;
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
    String endpoint;

    static boolean windows() {
        return System.getProperty("os.name").startsWith("Windows");
    }

    Nodes(Path root) throws Exception {
        this.root = root;
        logs = Files.createDirectories(root.resolve(".logs/run-" + runId));
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

    void start() throws Exception {
        if (!DOCKER) {
            composeStarted = true;
            compose("up", "-d", "--wait", "redis");
        }
        String redis = (DOCKER ? "redis:6379" : compose("port", "redis", "6379"));
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
                            role == NodeRole.SESSION ? "tcp://127.0.0.1:0" : null,
                            declared.dungeonCapacity());
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
        nodes.get(id).stop();
        startNode(id);
    }

    public void close() throws Exception {
        Exception failure = null;
        for (var node : new ArrayList<>(nodes.values()).reversed())
            try {
                node.cleanup();
            } catch (Exception error) {
                if (failure == null) failure = error;
                else failure.addSuppressed(error);
            }
        if (composeStarted)
            try {
                compose("down", "--volumes", "--remove-orphans");
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

    private static final class Node {
        private final String id;
        private final Process process;
        private final String container;
        private final BufferedWriter input;
        private final LinkedBlockingQueue<String> output = new LinkedBlockingQueue<>();

        Node(String id, String module, Path config, Path root, Path logs) throws Exception {
            this.id = id;
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
                                        var reader = process.inputReader()) {
                                    for (String line; (line = reader.readLine()) != null; ) {
                                        writer.write(line);
                                        writer.newLine();
                                        writer.flush();
                                        output.add(line);
                                    }
                                    output.add(Control.PROCESS_EXIT + process.waitFor());
                                } catch (Exception error) {
                                    output.add(Control.PROCESS_EXIT + error);
                                }
                            });
        }

        String waitFor(String prefix) throws Exception {
            long seconds =
                    prefix.equals(Control.SAFE_TO_SHUTDOWN)
                            ? Control.SAFE_DEADLINE.toSeconds()
                            : Control.PROCESS_SECONDS;
            long expires = System.nanoTime() + TimeUnit.SECONDS.toNanos(seconds);
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
            waitFor(Control.STOPPED);
            Structure.require(
                    process.waitFor(Control.PROCESS_SECONDS, TimeUnit.SECONDS),
                    id + " failed to exit");
            Structure.require(
                    process.exitValue() == 0, id + " shutdown exit: " + process.exitValue());
            if (container != null) docker("rm", container);
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
                        if (container != null) docker("rm", "-f", container);
                    }
                } catch (Exception | Error error) {
                    if (primary != null) primary.addSuppressed(error);
                    else throw error;
                }
            }
        }
    }
}
