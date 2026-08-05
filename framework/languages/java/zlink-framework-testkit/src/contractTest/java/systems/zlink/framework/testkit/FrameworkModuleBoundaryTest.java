package systems.zlink.framework.testkit;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.stream.Stream;
import org.junit.jupiter.api.Test;

final class FrameworkModuleBoundaryTest {
    private static final Set<String> EXPECTED_MODULES = Set.of(
        "zlink-framework-core",
        "zlink-framework-spring-boot-starter",
        "zlink-framework-codec-msgpack",
        "zlink-framework-codec-protobuf",
        "zlink-stream-connector",
        "zlink-framework-kotlin",
        "zlink-framework-testkit");

    private static final List<String> EXPECTED_ADDITIONAL_SOURCE_SET_DIRS = List.of(
        "src/contractTest/java",
        "src/fakeBackendTest/java",
        "src/integrationTest/java",
        "src/sampleTest/java");

    private static final Map<String, String> EXPECTED_PACKAGE_MARKERS = Map.ofEntries(
        Map.entry("zlink-framework-core", "systems/zlink/framework/package-info.java"),
        Map.entry("zlink-framework-spring-boot-starter", "systems/zlink/framework/spring/package-info.java"),
        Map.entry("zlink-framework-codec-msgpack", "systems/zlink/framework/codecs/msgpack/package-info.java"),
        Map.entry("zlink-framework-codec-protobuf", "systems/zlink/framework/codecs/protobuf/package-info.java"),
        Map.entry("zlink-stream-connector", "systems/zlink/stream/connector/package-info.java"),
        Map.entry("zlink-framework-kotlin", "systems/zlink/framework/kotlin/package-info.java"),
        Map.entry("zlink-framework-testkit", "systems/zlink/framework/testkit/package-info.java"));

    @Test
    void phase0ModulesMatchExecutionPlanTable() {
        Path root = frameworkJavaRoot();

        Set<String> actualModules = EXPECTED_MODULES.stream()
            .filter(module -> Files.isDirectory(root.resolve(module)))
            .collect(java.util.stream.Collectors.toSet());

        assertEquals(EXPECTED_MODULES, actualModules);
    }

    @Test
    void phase0SourceSetsAreDeclaredInBuildSkeleton() throws IOException {
        Path root = frameworkJavaRoot();
        String rootBuild = Files.readString(root.resolve("build.gradle.kts"));

        for (String sourceSetDir : EXPECTED_ADDITIONAL_SOURCE_SET_DIRS) {
            assertTrue(rootBuild.contains(sourceSetDir), "missing source set declaration for " + sourceSetDir);
        }
    }

    @Test
    void phase0PackageMarkersMatchPublicModuleBoundary() {
        Path root = frameworkJavaRoot();

        for (Map.Entry<String, String> marker : EXPECTED_PACKAGE_MARKERS.entrySet()) {
            Path markerPath = root.resolve(marker.getKey())
                .resolve("src/main/java")
                .resolve(marker.getValue());
            assertTrue(Files.isRegularFile(markerPath), "missing " + markerPath);
        }
    }

    @Test
    void frameworkPublicSourcesDoNotImportBindingRuntimePackages() throws IOException {
        Path root = frameworkJavaRoot();

        try (Stream<Path> files = Files.walk(root)) {
            List<Path> offenders = files
                .filter(Files::isRegularFile)
                .filter(path -> path.toString().endsWith(".java"))
                .filter(path -> path.toString().contains("/src/main/java/"))
                .filter(path -> !path.toString().contains("/systems/zlink/framework/runtime/"))
                .filter(FrameworkModuleBoundaryTest::importsBindingRuntimePackage)
                .toList();

            assertTrue(offenders.isEmpty(), "binding runtime imports in public framework sources: " + offenders);
        }
    }

    @Test
    void backendAdapterRuntimeImportsOnlyAllowedBindingPrimitives() throws IOException {
        Path runtimeRoot = frameworkJavaRoot()
            .resolve("zlink-framework-core")
            .resolve("src/main/java/systems/zlink/framework/runtime");
        Set<String> allowedImports = Set.of(
            "import systems.zlink.contracts.core.Context;",
            "import systems.zlink.contracts.core.RoutingId;",
            "import systems.zlink.contracts.core.Zlink;",
            "import systems.zlink.contracts.errors.ConfigResult;",
            "import systems.zlink.contracts.messaging.Message;",
            "import systems.zlink.contracts.errors.ZlinkConfigException;",
            "import systems.zlink.contracts.sockets.SendFlags;",
            "import systems.zlink.contracts.errors.ConnectResult;",
            "import systems.zlink.contracts.errors.ZlinkConnectException;",
            "import systems.zlink.contracts.errors.ZlinkCloseException;",
            "import systems.zlink.contracts.errors.ZlinkRecvException;",
            "import systems.zlink.contracts.errors.ZlinkRequestException;",
            "import systems.zlink.contracts.errors.ZlinkSubmitException;",
            "import systems.zlink.contracts.eventing.MonitorEvent;",
            "import systems.zlink.contracts.eventing.MonitorEventType;",
            "import systems.zlink.contracts.eventing.SocketMonitor;",
            "import systems.zlink.contracts.sockets.RequestResult;",
            "import systems.zlink.contracts.sockets.RecvFlags;",
            "import systems.zlink.contracts.sockets.RecvResult;",
            "import systems.zlink.contracts.sockets.SubmitResult;",
            "import systems.zlink.contracts.messaging.Received;",
            "import systems.zlink.contracts.messaging.TopicMessage;",
            "import systems.zlink.framework.runtime.internal.binding.spot.Actor;",
            "import systems.zlink.framework.runtime.internal.binding.spot.ActorBindOperation;",
            "import systems.zlink.framework.runtime.internal.binding.spot.ActorJoinCompletion;",
            "import systems.zlink.framework.runtime.internal.binding.spot.ActorJoinRequest;",
            "import systems.zlink.framework.runtime.internal.binding.spot.ActorJoinSubmitOperation;",
            "import systems.zlink.framework.runtime.internal.binding.spot.ActorReceived;",
            "import systems.zlink.framework.runtime.internal.binding.spot.ActorRef;",
            "import systems.zlink.framework.runtime.internal.binding.spot.ActorUnbindOperation;",
            "import systems.zlink.framework.runtime.internal.binding.spot.ReplyOperation;",
            "import systems.zlink.framework.runtime.internal.binding.spot.RequestOperation;",
            "import systems.zlink.framework.runtime.internal.binding.spot.SendOperation;",
            "import systems.zlink.framework.runtime.internal.binding.spot.Spot;",
            "import systems.zlink.framework.runtime.internal.binding.spot.SpotActorLifecycleEvent;",
            "import systems.zlink.framework.runtime.internal.binding.spot.SpotDispatchEvent;",
            "import systems.zlink.framework.runtime.internal.binding.spot.SpotDispatchInfo;",
            "import systems.zlink.framework.runtime.internal.binding.spot.SpotKind;",
            "import systems.zlink.framework.runtime.internal.binding.spot.SpotNode;",
            "import systems.zlink.framework.runtime.internal.binding.spot.SpotNodeMode;",
            "import systems.zlink.framework.runtime.internal.binding.spot.SpotNodePeerEntry;",
            "import systems.zlink.framework.runtime.internal.binding.spot.SpotNodeStatus;",
            "import systems.zlink.framework.runtime.internal.binding.spot.SpotNodeSubjectEntry;",
            "import systems.zlink.framework.runtime.internal.binding.spot.SpotRouteBridge;",
            "import systems.zlink.contracts.sockets.DealerSocket;",
            "import systems.zlink.contracts.sockets.PubSocket;",
            "import systems.zlink.contracts.sockets.RouterSocket;",
            "import systems.zlink.contracts.sockets.Socket;",
            "import systems.zlink.contracts.sockets.StreamSocket;",
            "import systems.zlink.contracts.sockets.SubSocket;");

        try (Stream<Path> files = Files.walk(runtimeRoot)) {
            List<String> offenders = files
                .filter(Files::isRegularFile)
                .filter(path -> path.toString().endsWith(".java"))
                .filter(path -> !path.toString().contains("/systems/zlink/framework/runtime/binding/"))
                .filter(path -> !path.toString().contains(
                    "/systems/zlink/framework/runtime/internal/binding/"))
                .flatMap(path -> bindingContractImports(path).stream())
                .filter(line -> !allowedImports.contains(line))
                .toList();

            assertTrue(offenders.isEmpty(), "disallowed binding imports in backend adapter: " + offenders);
        }
    }

    @Test
    void javaBackendAdapterImplementationUsesOnlyBindingPublicApi() throws IOException {
        Path bindingAdapterRoot = frameworkJavaRoot()
            .resolve("zlink-framework-core")
            .resolve("src/main/java/systems/zlink/framework/runtime/binding");

        if (!Files.isDirectory(bindingAdapterRoot)) {
            return;
        }

        try (Stream<Path> files = Files.walk(bindingAdapterRoot)) {
            List<Path> offenders = files
                .filter(Files::isRegularFile)
                .filter(path -> path.toString().endsWith(".java"))
                .filter(FrameworkModuleBoundaryTest::importsBindingRuntimePackage)
                .toList();

            assertTrue(offenders.isEmpty(), "binding runtime/internal imports in Java adapter: " + offenders);
        }
    }

    @Test
    void javaBackendAdapterDoesNotUseReflectionToReachBindingInternals() throws IOException {
        Path bindingAdapterRoot = frameworkJavaRoot()
            .resolve("zlink-framework-core")
            .resolve("src/main/java/systems/zlink/framework/runtime/binding");

        if (!Files.isDirectory(bindingAdapterRoot)) {
            return;
        }

        List<String> forbiddenSnippets = List.of(
            "java.lang.reflect",
            "getDeclared",
            "setAccessible",
            "MethodHandles",
            "VarHandle");

        try (Stream<Path> files = Files.walk(bindingAdapterRoot)) {
            List<String> offenders = files
                .filter(Files::isRegularFile)
                .filter(path -> path.toString().endsWith(".java"))
                .flatMap(path -> forbiddenReflectionUses(path, forbiddenSnippets).stream())
                .toList();

            assertTrue(offenders.isEmpty(), "reflection use in Java binding adapter: " + offenders);
        }
    }

    private static boolean importsBindingRuntimePackage(Path path) {
        try {
            String content = Files.readString(path);
            String withoutPublicNativeFacade = content.replace(
                "import systems.zlink.runtime.nativeapi.Native;",
                "");
            return withoutPublicNativeFacade.contains("import systems.zlink.runtime.")
                || content.contains("import systems.zlink.internal.");
        } catch (IOException ex) {
            throw new IllegalStateException("failed to read " + path, ex);
        }
    }

    private static List<String> bindingContractImports(Path path) {
        try {
            return Files.readAllLines(path)
                .stream()
                .filter(line -> line.startsWith("import systems.zlink.contracts."))
                .toList();
        } catch (IOException ex) {
            throw new IllegalStateException("failed to read " + path, ex);
        }
    }

    private static List<String> forbiddenReflectionUses(Path path, List<String> forbiddenSnippets) {
        try {
            return Files.readAllLines(path)
                .stream()
                .filter(line -> forbiddenSnippets.stream().anyMatch(line::contains))
                .map(line -> path + ": " + line.trim())
                .toList();
        } catch (IOException ex) {
            throw new IllegalStateException("failed to read " + path, ex);
        }
    }

    private static Path frameworkJavaRoot() {
        return Path.of(System.getProperty("user.dir")).getParent();
    }
}
