package systems.zlink.framework.testkit;

import static org.junit.jupiter.api.Assertions.assertAll;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.regex.Pattern;
import java.util.stream.Stream;
import org.junit.jupiter.api.Test;

final class SampleReleaseGateContractTest {
    private static final Set<String> REQUIRED_LANGUAGES = Set.of("java", "kotlin");
    private static final String FORBIDDEN_SAMPLE_ASYNC_HELPER = "Sample" + "Async";
    private static final String FORBIDDEN_TICTACTOE_RESULT = "TicTacToeClient" + "Result";

    private static final Set<String> REQUIRED_SAMPLES = Set.of(
        "TicTacToe",
        "Bingo",
        "SupportChat",
        "DeliveryDispatch",
        "ShoppingMall",
        "GameQuest");

    private static final Set<String> REQUIRED_RUNTIME_PACKAGES = Set.of(
        "actors",
        "binding",
        "channels",
        "configuration",
        "diagnostics",
        "handlers",
        "host",
        "internal",
        "locations",
        "mesh",
        "messaging",
        "metrics",
        "monitoring",
        "spots",
        "streams");

    private static final List<String> DOTNET_DIRECT_SAMPLE_PROJECTS = List.of(
        "Client",
        "Server",
        "Shared");

    private static final List<String> DOTNET_BINGO_SAMPLE_PROJECTS = List.of(
        "Client",
        "Server/Api",
        "Server/Matchmaking",
        "Server/Play",
        "Server/Session",
        "Shared");

    private static final List<String> FORBIDDEN_SAMPLE_PATTERNS = List.of(
        "import systems.zlink.runtime.",
        "import systems.zlink.internal.",
        "systems.zlink.contracts.core.RoutingId.",
        "MetadataStore",
        "BingoNotificationLoopbackServer",
        "BingoRoomState room",
        "new BingoRoomState",
        "CountDownLatch",
        "Thread.sleep",
        "sleep(",
        "System.in.read",
        FORBIDDEN_SAMPLE_ASYNC_HELPER,
        "session relay JSON",
        "in-memory route channel replacement");

    private static final Pattern FORBIDDEN_KOTLIN_HANDLER_REGISTRATION =
        Pattern.compile("(?s)handlers\\(\\)\\s*\\.\\s*"
            + "((addPacket|addActorPacket|addActorSend|addActorRequest|addSubscribe)\\s*(<|\\()"
            + "|addHandler\\s*\\([^)]*::class\\.java)");
    private static final Pattern JAVA_MANUAL_SPOT_HANDLER_REGISTRATION =
        Pattern.compile("(?s)handlers\\(\\)\\s*\\.\\s*"
            + "(addHandler|addPacket|addActorPacket|addActorSend|addActorRequest|addSubscribe)\\s*\\(");
    private static final Pattern KOTLIN_MANUAL_SPOT_HANDLER_REGISTRATION =
        Pattern.compile("(?s)handlers\\(\\)\\s*\\.\\s*addHandler\\s*<");

    @Test
    void requiredSamplesExposeExecutableEntryPoints() throws IOException {
        Path samplesRoot = samplesRoot();

        assertTrue(Files.isRegularFile(samplesRoot.resolve("run_samples.sh")),
            "missing aggregate sample runner");
        assertTrue(Files.isExecutable(samplesRoot.resolve("run_samples.sh")),
            "aggregate sample runner must be executable");
        assertTrue(Files.isRegularFile(samplesRoot.resolve("run_samples.ps1")),
            "missing aggregate PowerShell sample runner");
        String aggregateRunner = Files.readString(samplesRoot.resolve("run_samples.sh"));
        assertTrue(aggregateRunner.contains("ZLINK_LIBRARY_PATH")
                && aggregateRunner.contains("core/build/lib/libzlink.so"),
            "aggregate sample runner must use the local core runtime when it is available");
        String aggregatePowerShellRunner = Files.readString(samplesRoot.resolve("run_samples.ps1"));
        assertTrue(aggregatePowerShellRunner.contains("ZLINK_LIBRARY_PATH")
                && aggregatePowerShellRunner.contains("core/build/lib/libzlink.so"),
            "aggregate PowerShell sample runner must use the local core runtime when it is available");

        for (String language : REQUIRED_LANGUAGES) {
            Path languageRoot = samplesRoot.resolve(language);
            assertTrue(Files.isDirectory(languageRoot), "missing sample language directory " + language);

            for (String sample : REQUIRED_SAMPLES) {
                Path sampleRoot = languageRoot.resolve(sample);
                String sampleName = language + "/" + sample;
                assertTrue(Files.isDirectory(sampleRoot), "missing sample " + sampleName);
                assertFalse(Files.exists(sampleRoot.resolve("settings.gradle.kts")),
                    "nested settings.gradle.kts makes IntelliJ import duplicate Gradle roots for " + sampleName);
                assertTrue(Files.isRegularFile(sampleRoot.resolve("standalone.settings.gradle.kts")),
                    "missing standalone.settings.gradle.kts for " + sampleName);
                assertTrue(Files.isRegularFile(sampleRoot.resolve("build.gradle.kts")),
                    "missing build.gradle.kts for " + sampleName);
                assertTrue(Files.isRegularFile(sampleRoot.resolve("run_sample.sh")),
                    "missing run_sample.sh for " + sampleName);
                assertTrue(Files.isExecutable(sampleRoot.resolve("run_sample.sh")),
                    "run_sample.sh must be executable for " + sampleName);
                String runner = Files.readString(sampleRoot.resolve("run_sample.sh"));
                assertTrue(runner.contains("--settings-file standalone.settings.gradle.kts"),
                    "run_sample.sh must use standalone settings for " + sampleName);
                Path powerShellRunnerPath = sampleRoot.resolve("run_sample.ps1");
                if (Files.isRegularFile(powerShellRunnerPath)) {
                    String powerShellRunner = Files.readString(powerShellRunnerPath);
                    assertTrue(powerShellRunner.contains("--settings-file")
                            && powerShellRunner.contains("standalone.settings.gradle.kts"),
                        "run_sample.ps1 must use standalone settings for " + sampleName);
                }
            }

            assertDotNetProjectLayout(languageRoot.resolve("TicTacToe"), DOTNET_DIRECT_SAMPLE_PROJECTS);
            assertDotNetProjectLayout(languageRoot.resolve("Bingo"), DOTNET_BINGO_SAMPLE_PROJECTS);
            assertSharedProjectContainsOnlyContracts(languageRoot.resolve("TicTacToe"));
            assertSharedProjectContainsOnlyContracts(languageRoot.resolve("Bingo"));
        }
    }

    @Test
    void redisBackedSamplesCreateOneDedicatedContainerPerRun() throws IOException {
        Path commonRunner = samplesRoot().resolve("runner-common.sh");
        String helper = Files.readString(commonRunner);
        for (String needle : List.of(
                "docker create",
                "--tmpfs /data",
                "127.0.0.1::6379",
                "docker start",
                "docker inspect",
                "docker rm -fv")) {
            assertTrue(helper.contains(needle), "Redis helper must contain " + needle);
        }

        String powerShellHelper = Files.readString(samplesRoot().resolve("redis-common.ps1"));
        for (String needle : List.of(
                "ProcessStartInfo",
                "WaitForExit",
                "redis-cli", "ping",
                "create", "--tmpfs", "127.0.0.1::6379",
                "start", "inspect", "rm", "-fv")) {
            assertTrue(powerShellHelper.contains(needle),
                "PowerShell Redis helper must contain " + needle);
        }

        for (String language : REQUIRED_LANGUAGES) {
            for (String sample : REQUIRED_SAMPLES) {
                Path runnerPath = samplesRoot().resolve(language).resolve(sample).resolve("run_sample.sh");
                String runner = Files.readString(runnerPath);
                String expectedScope = "zlink-redis-" + language + "-sample-"
                    + sample.toLowerCase(java.util.Locale.ROOT);
                assertTrue(runner.contains("zlink_redis_start_scoped_assign"),
                    language + "/" + sample + " must create its own Redis container");
                assertTrue(runner.contains(expectedScope),
                    language + "/" + sample + " must identify its Redis container scope");
                assertFalse(Pattern.compile("(?m)^\\s*if\\s+\\[\\[[^\\n]*[A-Z]+_REDIS_ENDPOINT")
                        .matcher(runner).find(),
                    language + "/" + sample + " must not reuse an external Redis endpoint");
                assertFalse(Pattern.compile("(?m)^\\s*docker\\s+(create|start|inspect|rm|run)\\b")
                        .matcher(runner).find(),
                    language + "/" + sample
                        + " must use the shared helper instead of assembling Docker commands");
                assertFalse(Pattern.compile("(?m)^\\s*sleep\\s+[1-9][0-9]*(?:\\s|$)")
                        .matcher(runner).find(),
                    language + "/" + sample
                        + " must use observable readiness instead of a fixed multi-second sleep");

                Path powerShellRunnerPath = runnerPath.resolveSibling("run_sample.ps1");
                if (Files.isRegularFile(powerShellRunnerPath)) {
                    String powerShellRunner = Files.readString(powerShellRunnerPath);
                    assertTrue(powerShellRunner.contains("Start-ZlinkSampleRedis")
                            && powerShellRunner.contains("Remove-ZlinkSampleRedis"),
                        language + "/" + sample
                            + " PowerShell runner must use the shared Redis lifecycle helper");
                    assertFalse(powerShellRunner.contains("& docker"),
                        language + "/" + sample
                            + " PowerShell runner must not assemble Docker commands directly");
                    assertFalse(Pattern.compile(
                            "(?im)^\\s*Start-Sleep\\s+(?:-Seconds\\s+)?[1-9][0-9]*(?:\\s|$)")
                            .matcher(powerShellRunner).find(),
                        language + "/" + sample
                            + " PowerShell runner must use observable readiness instead of a fixed sleep");
                }
            }
        }
    }

    @Test
    void commonSampleMessageNamesRemainVisibleInJavaAndKotlinSources() throws IOException {
        Path javaTicTacToe = samplesRoot().resolve("java/TicTacToe");
        Path kotlinTicTacToe = samplesRoot().resolve("kotlin/TicTacToe");
        assertSourceContains(javaTicTacToe, ".java", "LeaveGameReq");
        assertSourceContains(kotlinTicTacToe, ".kt", "LeaveGameReq");
        assertSourceDoesNotContain(javaTicTacToe, ".java", "LeaveGameMsg");
        assertSourceDoesNotContain(kotlinTicTacToe, ".kt", "LeaveGameMsg");

        Path javaDelivery = samplesRoot().resolve("java/DeliveryDispatch");
        for (String messageName : List.of(
                "CreateDeliveryReq", "CreateDeliveryRes",
                "SubscribeDeliveryReq", "SubscribeDeliveryRes",
                "AssignDeliveryMsg", "BindCourierSessionReq", "BindCourierSessionRes",
                "BindCourierReq", "BindCourierRes",
                "FindCourierActorReq", "FindCourierActorRes",
                "EnsureCourierActorReq", "EnsureCourierActorRes",
                "OfferDeliveryMsg", "OfferDeliveryResultMsg",
                "DeliveryStatusChangedReq", "DeliveryStatusChangedRes",
                "FindCustomerActorReq", "FindCustomerActorRes",
                "EnsureCustomerActorReq", "EnsureCustomerActorRes")) {
            assertSourceContains(javaDelivery.resolve("Shared"), ".java", messageName);
        }

        Path kotlinDelivery = samplesRoot().resolve("kotlin/DeliveryDispatch");
        for (String messageName : List.of(
                "CreateDeliveryReq", "CreateDeliveryRes",
                "SubscribeDeliveryReq", "SubscribeDeliveryRes",
                "AssignDeliveryMsg", "BindCourierSessionReq", "BindCourierSessionRes",
                "BindCourierReq", "BindCourierRes",
                "FindCourierActorReq", "FindCourierActorRes",
                "EnsureCourierActorReq", "EnsureCourierActorRes",
                "OfferDeliveryMsg", "OfferDeliveryResultMsg",
                "DeliveryStatusChangedReq", "DeliveryStatusChangedRes",
                "FindCustomerActorReq", "FindCustomerActorRes",
                "EnsureCustomerActorReq", "EnsureCustomerActorRes")) {
            assertSourceContains(kotlinDelivery.resolve("Shared"), ".kt", messageName);
        }
        assertSourceContains(javaDelivery.resolve("Shared"), ".java", "ActorRefSnapshot");
        assertSourceContains(kotlinDelivery.resolve("Shared"), ".kt", "ActorRefSnapshot");
        assertSourceDoesNotContain(javaDelivery, ".java", "ActorRefWire");
        assertSourceDoesNotContain(kotlinDelivery, ".kt", "ActorRefWire");
        assertSourceDoesNotContain(kotlinDelivery, ".kt", "data class AssignDelivery(");

        Path javaSupportChat = samplesRoot().resolve("java/SupportChat");
        Path kotlinSupportChat = samplesRoot().resolve("kotlin/SupportChat");
        for (String messageName : List.of(
                "AuthenticateReq", "AuthenticateRes",
                "OpenConversationApiReq", "OpenConversationApiRes",
                "AllocateConversationReq", "AllocateConversationRes",
                "EnsureSupportUserActorReq", "EnsureSupportUserActorRes",
                "EnsureAgentConversationReq", "EnsureAgentConversationRes",
                "OpenConversationReq", "OpenConversationRes",
                "SetAgentAvailableReq", "SetAgentAvailableRes",
                "JoinConversationReq", "JoinConversationRes",
                "SendChatMessageReq", "SendChatMessageRes",
                "SetTypingReq", "CloseConversationReq", "CloseConversationRes",
                "ParticipantJoinedNotify", "ConversationAssignedNotify",
                "ChatMessageNotify", "TypingChangedNotify",
                "ConversationIdleNotify", "ConversationClosedNotify")) {
            assertSourceContains(javaSupportChat.resolve("Shared"), ".java", messageName);
            assertSourceContains(kotlinSupportChat.resolve("Shared"), ".kt", messageName);
        }
        assertSourceContains(javaSupportChat.resolve("Shared"), ".java", "ActorRefSnapshot");
        assertSourceContains(kotlinSupportChat.resolve("Shared"), ".kt", "ActorRefSnapshot");
        for (String obsoleteName : List.of(
                "ActorRefWire", "JoinConversationSupportReq", "SendChatMessageSupportReq",
                "SetTypingSupportReq", "CloseConversationSupportReq", "ServerAssertionRequest")) {
            assertSourceDoesNotContain(javaSupportChat, ".java", obsoleteName);
            assertSourceDoesNotContain(kotlinSupportChat, ".kt", obsoleteName);
        }
        String javaSupportContracts = Files.readString(javaSupportChat.resolve(
            "Shared/src/main/java/systems/zlink/samples/supportchat/shared/contracts/Messages.java"));
        assertTrue(javaSupportContracts.contains("record SendChatMessageReq(String text)")
                && javaSupportContracts.contains("record SetTypingReq(boolean isTyping)")
                && javaSupportContracts.contains("record CloseConversationReq(String reason)"),
            "Java conversation-scoped payloads must route ConversationId through metadata");
        String kotlinSupportContracts = Files.readString(kotlinSupportChat.resolve(
            "Shared/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/shared/contracts/Messages.kt"));
        assertTrue(Pattern.compile("(?s)data class SendChatMessageReq\\(\\s*val text: String,?\\s*\\)")
                .matcher(kotlinSupportContracts).find()
                && Pattern.compile("(?s)data class SetTypingReq\\(\\s*val isTyping: Boolean,?\\s*\\)")
                    .matcher(kotlinSupportContracts).find()
                && Pattern.compile("(?s)data class CloseConversationReq\\(\\s*val reason: String\\?,?\\s*\\)")
                    .matcher(kotlinSupportContracts).find(),
            "Kotlin conversation-scoped payloads must route ConversationId through metadata");

        Path javaGameQuest = samplesRoot().resolve("java/GameQuest");
        Path kotlinGameQuest = samplesRoot().resolve("kotlin/GameQuest");
        assertSourceContains(javaGameQuest.resolve("Shared"), ".java", "GameplayMsg");
        assertSourceContains(kotlinGameQuest.resolve("Shared"), ".kt", "GameplayMsg");
        assertSourceDoesNotContain(javaGameQuest, ".java", "GameplayEventEnvelope");
        assertSourceDoesNotContain(kotlinGameQuest, ".kt", "GameplayEventEnvelope");
    }

    @Test
    void roleBasedSamplesDoNotCollapseIntoSingleGradleRun() throws IOException {
        for (String language : REQUIRED_LANGUAGES) {
            for (String sample : List.of("TicTacToe", "Bingo")) {
                Path sampleRoot = samplesRoot().resolve(language).resolve(sample);
                String script = Files.readString(sampleRoot.resolve("run_sample.sh"));
                String powerShellScript = Files.readString(sampleRoot.resolve("run_sample.ps1"));
                assertFalse(script.matches("(?s).*\\n\\s*gradle\\s+run\\s+--quiet\\s*\\n?.*"),
                    language + "/" + sample + " must start the same role entry points as the .NET sample");
                assertTrue(script.contains(":Client:run")
                        || script.contains(":Client:installDist")
                        || script.contains("Client/build/install")
                        || script.contains("/Client/"),
                    language + "/" + sample + " runner must execute a distinct Client role");
                assertTrue(powerShellScript.contains(":Client:run")
                        || powerShellScript.contains(":Client:installDist")
                        || powerShellScript.contains("Client/build/install")
                        || powerShellScript.contains("/Client/"),
                    language + "/" + sample + " PowerShell runner must execute a distinct Client role");
                if (!sample.equals("TicTacToe")) {
                    assertTrue(script.contains(":Server:Api:run")
                            || script.contains(":Server:Api:installDist")
                            || script.contains("Server/Api/build/install")
                            || script.contains("/Server/Api/"),
                        language + "/" + sample + " runner must execute a distinct Api role");
                    assertTrue(script.contains(":Server:Play:run")
                            || script.contains(":Server:Play:installDist")
                            || script.contains("Server/Play/build/install")
                            || script.contains("/Server/Play/"),
                        language + "/" + sample + " runner must execute a distinct Play role");
                    assertTrue(script.contains(":Server:Session:run")
                            || script.contains(":Server:Session:installDist")
                            || script.contains("Server/Session/build/install")
                            || script.contains("/Server/Session/"),
                        language + "/" + sample + " runner must execute a distinct Session role");
                    assertTrue(powerShellScript.contains(":Server:Api:run")
                            || powerShellScript.contains(":Server:Api:installDist")
                            || powerShellScript.contains("Server/Api/build/install")
                            || powerShellScript.contains("/Server/Api/"),
                        language + "/" + sample + " PowerShell runner must execute a distinct Api role");
                    assertTrue(powerShellScript.contains(":Server:Play:run")
                            || powerShellScript.contains(":Server:Play:installDist")
                            || powerShellScript.contains("Server/Play/build/install")
                            || powerShellScript.contains("/Server/Play/"),
                        language + "/" + sample + " PowerShell runner must execute a distinct Play role");
                    assertTrue(powerShellScript.contains(":Server:Session:run")
                            || powerShellScript.contains(":Server:Session:installDist")
                            || powerShellScript.contains("Server/Session/build/install")
                            || powerShellScript.contains("/Server/Session/"),
                        language + "/" + sample + " PowerShell runner must execute a distinct Session role");
                }
            }
        }
    }

    @Test
    void sampleSourcesUseOnlyPublicFrameworkAndConnectorApi() throws IOException {
        try (Stream<Path> files = Files.walk(samplesRoot())) {
            Map<Path, List<String>> offenders = files
                .filter(Files::isRegularFile)
                .filter(SampleReleaseGateContractTest::isSampleSource)
                .map(path -> Map.entry(path, forbiddenLines(path)))
                .filter(entry -> !entry.getValue().isEmpty())
                .collect(java.util.stream.Collectors.toMap(
                    Map.Entry::getKey,
                    Map.Entry::getValue));

            assertTrue(offenders.isEmpty(), "sample forbidden pattern offenders: " + offenders);
        }
    }

    @Test
    void kotlinSamplesAndE2eUseAddHandlerReifiedRegistrationOnly() throws IOException {
        Map<Path, List<String>> offenders = new java.util.LinkedHashMap<>();
        for (Path root : List.of(samplesRoot().resolve("kotlin"), frameworkJavaRoot().resolve("e2e-kotlin"))) {
            try (Stream<Path> files = Files.walk(root)) {
                files
                    .filter(Files::isRegularFile)
                    .filter(path -> path.toString().endsWith(".kt"))
                    .filter(SampleReleaseGateContractTest::isSampleSource)
                    .map(path -> Map.entry(path, forbiddenKotlinHandlerRegistrations(path)))
                    .filter(entry -> !entry.getValue().isEmpty())
                    .forEach(entry -> offenders.put(entry.getKey(), entry.getValue()));
            }
        }

        assertTrue(offenders.isEmpty(),
            "Kotlin sample/e2e spot handler registration must use only "
                + "context.handlers().addHandler<MyHandler>(): " + offenders);
    }

    @Test
    void javaSamplesUseAutomaticSpotHandlerRegistration() throws IOException {
        Map<Path, List<String>> offenders = new java.util.LinkedHashMap<>();
        try (Stream<Path> files = Files.walk(samplesRoot().resolve("java"))) {
            files
                .filter(Files::isRegularFile)
                .filter(path -> path.toString().endsWith(".java"))
                .filter(SampleReleaseGateContractTest::isSampleSource)
                .map(path -> Map.entry(path, javaManualSpotHandlerRegistrations(path)))
                .filter(entry -> !entry.getValue().isEmpty())
                .forEach(entry -> offenders.put(entry.getKey(), entry.getValue()));
        }

        assertTrue(offenders.isEmpty(),
            "Java samples must use addHandlersFromPackageOf automatic registration: "
                + offenders);
    }

    @Test
    void kotlinSamplesUseAutomaticSpotHandlerRegistration() throws IOException {
        Map<Path, List<String>> offenders = new java.util.LinkedHashMap<>();
        try (Stream<Path> files = Files.walk(samplesRoot().resolve("kotlin"))) {
            files
                .filter(Files::isRegularFile)
                .filter(path -> path.toString().endsWith(".kt"))
                .filter(SampleReleaseGateContractTest::isSampleSource)
                .map(path -> Map.entry(path, kotlinManualSpotHandlerRegistrations(path)))
                .filter(entry -> !entry.getValue().isEmpty())
                .forEach(entry -> offenders.put(entry.getKey(), entry.getValue()));
        }

        assertTrue(offenders.isEmpty(),
            "Kotlin samples must use addHandlersFromPackageOf automatic registration: "
                + offenders);
    }

    @Test
    void kotlinTicTacToeUsesAutomaticHandlerRegistration() throws IOException {
        String apiSource = sampleKotlinSource(
            "TicTacToe",
            "Server/src/main/kotlin",
            "systems/zlink/samples/kotlin/tictactoe/server/api/ApiServer.kt");
        String playSource = sampleKotlinSource(
            "TicTacToe",
            "Server/src/main/kotlin",
            "systems/zlink/samples/kotlin/tictactoe/server/play/PlayServer.kt");
        String entrySpotSource = sampleKotlinSource(
            "TicTacToe",
            "Server/src/main/kotlin",
            "systems/zlink/samples/kotlin/tictactoe/server/play/infrastructure/zlink/spots/entryspot/PlayEntrySpot.kt");
        String gameSpotSource = sampleKotlinSource(
            "TicTacToe",
            "Server/src/main/kotlin",
            "systems/zlink/samples/kotlin/tictactoe/server/play/infrastructure/zlink/spots/tictactoegamespot/TicTacToeGame.kt");
        String authHandlerSource = sampleKotlinSource(
            "TicTacToe",
            "Server/src/main/kotlin",
            "systems/zlink/samples/kotlin/tictactoe/server/api/handlers/AuthenticatePlayerHandler.kt");
        String createHandlerSource = sampleKotlinSource(
            "TicTacToe",
            "Server/src/main/kotlin",
            "systems/zlink/samples/kotlin/tictactoe/server/api/handlers/CreateGameHttpHandler.kt");

        assertAll(
            () -> assertTrue(apiSource.contains("addHandlersFromPackageOf")),
            () -> assertFalse(apiSource.contains("addRequestHandler(")),
            () -> assertTrue(playSource.contains("addHandlersFromPackageOf")),
            () -> assertFalse(playSource.contains("addRequestHandler(")),
            () -> assertTrue(playSource.contains("addRouteMesh(SampleNames.SpotMesh)")),
            () -> assertFalse(playSource.contains("addSpotMesh(")),
            () -> assertTrue(playSource.contains("node.listen(routeEndpoint)")),
            () -> assertTrue(playSource.contains("node.channelName(SampleNames.PlayNode)")),
            () -> assertTrue(playSource.contains("node.peerConnections().connect(")),
            () -> assertFalse(playSource.contains("configureEntrySpot()")),
            () -> assertTrue(playSource.contains(".enableActorDispatch()")),
            () -> assertFalse(entrySpotSource.contains("handlers().add")),
            () -> assertFalse(gameSpotSource.contains("handlers().add")),
            () -> assertTrue(authHandlerSource.contains(
                "ZLinkSuspendingRequestHandler<AuthenticatePlayerReq, AuthenticatePlayerRes>")),
            () -> assertTrue(createHandlerSource.contains("ZLinkSpotManager")),
            () -> assertTrue(createHandlerSource.contains(".create(\"tictactoe.game\")")));
    }


    @Test
    void officialDocsKeepActorDestroyEntryOwned() throws IOException {
        List<Path> docs = officialActorDestroyDocs();
        List<String> offenders = new java.util.ArrayList<>();
        List<String> forbidden = List.of(
            "DestroyActorAsync",
            "destroyActorAsync",
            "destroy_actor",
            "OnActorLeft",
            "onActorLeft",
            "on_actor_left",
            "OnCreateActor",
            "on_actor_created",
            "onPostActorJoined",
            "disconnect -> destroy",
            "자동 삭제",
            "disconnect가 actor destroy를 실행한다",
            "disconnect가 actor destroy를 자동",
            "disconnect cleanup만으로 actor destroy가 실행된다",
            "destroy를 자동으로 실행한다");

        for (Path doc : docs) {
            String content = Files.readString(doc);
            for (String needle : forbidden) {
                if (content.contains(needle)) {
                    offenders.add(frameworkJavaRoot().relativize(doc) + ": " + needle);
                }
            }
        }

        String actorExact = Files.readString(frameworkJavaRoot().resolve(
            "../../doc/framework/common/spec/server/languages/java/interfaces/actors.ko.md"));
        assertTrue(actorExact.contains(
                "destroy(systems.zlink.framework.actors.ActorRef)"),
            "Java exact contract must keep Actor Manager destroy on exact ActorRef");
        assertTrue(actorExact.contains(
                "Destroy와 session bind만 exact ref를 받는다"),
            "Java exact contract must keep the exact-ref boundary explicit");
        assertTrue(offenders.isEmpty(), "actor destroy documentation offenders: " + offenders);
    }

    @Test
    void serverRoleSamplesRunZLinkThroughSpringLifecycle() throws IOException {
        for (String language : REQUIRED_LANGUAGES) {
            for (String sample : List.of("TicTacToe", "Bingo")) {
                Path sampleRoot = samplesRoot().resolve(language).resolve(sample);
                try (Stream<Path> files = Files.walk(sampleRoot)) {
                    Map<Path, List<String>> directStarts = files
                        .filter(Files::isRegularFile)
                        .filter(SampleReleaseGateContractTest::isSampleSource)
                        .filter(SampleReleaseGateContractTest::isServerRoleSource)
                        .map(path -> Map.entry(path, forbiddenDirectServerStarts(path)))
                        .filter(entry -> !entry.getValue().isEmpty())
                        .collect(java.util.stream.Collectors.toMap(
                            Map.Entry::getKey,
                            Map.Entry::getValue));

                    assertTrue(directStarts.isEmpty(),
                        language + "/" + sample + " server roles must not start ZLink directly: "
                            + directStarts);
                }

                try (Stream<Path> files = Files.walk(sampleRoot)) {
                    List<Path> nonSpringHosts = files
                        .filter(Files::isRegularFile)
                        .filter(SampleReleaseGateContractTest::isSampleSource)
                        .filter(SampleReleaseGateContractTest::isServerRoleSource)
                        .filter(path -> path.getFileName().toString().contains("HostFactory"))
                        .filter(SampleReleaseGateContractTest::isNotSpringBootHostFactory)
                        .toList();

                    assertTrue(nonSpringHosts.isEmpty(),
                        language + "/" + sample
                            + " server role Application classes must use Spring Boot lifecycle: "
                            + nonSpringHosts);
                }
            }
        }
    }

    @Test
    void frameworkRuntimeSourcesStaySplitByDotNetRuntimeCategories() throws IOException {
        Path runtimeRoot = frameworkJavaRoot()
            .resolve("zlink-framework-core/src/main/java/systems/zlink/framework/runtime");

        for (String runtimePackage : REQUIRED_RUNTIME_PACKAGES) {
            assertTrue(Files.isDirectory(runtimeRoot.resolve(runtimePackage)),
                "missing runtime package " + runtimePackage);
        }

        try (Stream<Path> files = Files.list(runtimeRoot)) {
            List<Path> rootJavaFiles = files
                .filter(Files::isRegularFile)
                .filter(path -> path.getFileName().toString().endsWith(".java"))
                .filter(path -> !path.getFileName().toString().equals("package-info.java"))
                .toList();

            assertTrue(rootJavaFiles.isEmpty(),
                "runtime root must not contain implementation files: " + rootJavaFiles);
        }
    }

    @Test
    void ticTacToeDirectSampleUsesFrameworkRuntimePublicFacade() throws IOException {
        SampleSourcePaths paths = javaSamplePaths("tictactoe");
        assertNoSampleSourcesUnder("java", "TicTacToe", "src/main/java",
            List.of(
                "systems/zlink/samples/tictactoe/client",
                "systems/zlink/samples/tictactoe/server",
                "systems/zlink/samples/tictactoe/shared"));
        assertSampleFilesExist("java", "TicTacToe", "Client/src/main/java", List.of(
            "systems/zlink/samples/tictactoe/client/TicTacToeClientArguments.java",
            "systems/zlink/samples/tictactoe/client/TicTacToeClientScenario.java",
            "systems/zlink/samples/tictactoe/client/TicTacToeClientOptions.java",
            "systems/zlink/samples/tictactoe/client/TicTacToeSampleDefaults.java"));
        assertTrue(sampleFileContains("java", "TicTacToe", "Client/src/main/java",
                "systems/zlink/samples/tictactoe/client/Program.java", "tictactoe=completed"),
            "Java TicTacToe Client role must report the same completion marker as other clients");
        assertTrue(sampleFileContains("java", "TicTacToe", "Client",
                "README.md", "Tic Tac Toe Client"),
            "Java TicTacToe Client role must include a standalone README");
        assertSampleFilesExist("java", "TicTacToe", "Server/src/main/java", List.of(
            "systems/zlink/samples/tictactoe/server/api/ApiServer.java",
            "systems/zlink/samples/tictactoe/server/api/ApiServerApplication.java",
            "systems/zlink/samples/tictactoe/server/api/handlers/AuthenticatePlayerHandler.java",
            "systems/zlink/samples/tictactoe/server/api/handlers/CreateGameHttpHandler.java",
            "systems/zlink/samples/tictactoe/server/configuration/SampleLogging.java",
            "systems/zlink/samples/tictactoe/server/configuration/SampleNames.java",
            "systems/zlink/samples/tictactoe/server/configuration/ApiSettings.java",
            "systems/zlink/samples/tictactoe/server/configuration/PlaySettings.java",
            "systems/zlink/samples/tictactoe/server/configuration/SampleConfigPath.java",
            "systems/zlink/samples/tictactoe/server/play/PlayServer.java",
            "systems/zlink/samples/tictactoe/server/play/PlayServerApplication.java",
            "systems/zlink/samples/tictactoe/server/play/infrastructure/zlink/actors/PlayActor.java",
            "systems/zlink/samples/tictactoe/server/play/infrastructure/zlink/actors/PlayActorFactory.java",
            paths.playSpot("entryspot", "PlayEntrySpot"),
            paths.playSpot("tictactoegamespot", "TicTacToeGame"),
            paths.playSpotHandler("tictactoegamespot", "PlayActorPlaceMarkHandler"),
            paths.playSpotHandler("tictactoegamespot", "TicTacToeGameTimerHandler"),
            "systems/zlink/samples/tictactoe/server/play/infrastructure/zlink/sessions/PlaySession.java"));
        assertSampleFilesExist("java", "TicTacToe", "Shared/src/main/java", List.of(
            "systems/zlink/samples/tictactoe/shared/contracts/GameState.java",
            "systems/zlink/samples/tictactoe/shared/contracts/GameStateNotify.java",
            "systems/zlink/samples/tictactoe/shared/contracts/AuthenticatePlayerReq.java",
            "systems/zlink/samples/tictactoe/shared/contracts/AuthenticatePlayerRes.java",
            "systems/zlink/samples/tictactoe/shared/contracts/AuthenticateReq.java",
            "systems/zlink/samples/tictactoe/shared/contracts/AuthenticateRes.java",
            "systems/zlink/samples/tictactoe/shared/contracts/CreateGameHttpReq.java",
            "systems/zlink/samples/tictactoe/shared/contracts/CreateGameHttpRes.java",
            "systems/zlink/samples/tictactoe/shared/contracts/JoinGameReq.java",
            "systems/zlink/samples/tictactoe/shared/contracts/JoinGameRes.java",
            "systems/zlink/samples/tictactoe/shared/contracts/PlaceMarkReq.java",
            "systems/zlink/samples/tictactoe/shared/contracts/PlaceMarkRes.java",
            "systems/zlink/samples/tictactoe/shared/contracts/PlayerJoinedNotify.java",
            "systems/zlink/samples/tictactoe/shared/contracts/TicTacToeGameJoinReq.java",
            "systems/zlink/samples/tictactoe/shared/contracts/TicTacToeGameJoinRes.java"));
        assertTrue(sampleFileContains("java", "TicTacToe", "Client/src/main/java",
                "systems/zlink/samples/tictactoe/client/Program.java", "TicTacToeClientArguments.parse"),
            "Java TicTacToe Client role Program must live in the Client project folder");
        String apiProgramSource = sampleJavaSource(
            "TicTacToe",
            "Server/src/main/java",
            "systems/zlink/samples/tictactoe/server/api/ApiProgram.java");
        String playProgramSource = sampleJavaSource(
            "TicTacToe",
            "Server/src/main/java",
            "systems/zlink/samples/tictactoe/server/play/PlayProgram.java");
        String serverBuildSource = sampleFile(
            "java",
            "TicTacToe",
            "Server",
            "build.gradle.kts");
        String clientSource = sampleJavaSource(
            "TicTacToe",
            "Client/src/main/java",
            "systems/zlink/samples/tictactoe/client/TicTacToeClientScenario.java");
        String clientProgramSource = sampleJavaSource(
            "TicTacToe",
            "Client/src/main/java",
            "systems/zlink/samples/tictactoe/client/Program.java");
        String apiSource = sampleJavaSource(
            "TicTacToe",
            "Server/src/main/java",
            "systems/zlink/samples/tictactoe/server/api/ApiServer.java");
        String apiHostFactorySource = sampleJavaSource(
            "TicTacToe",
            "Server/src/main/java",
            "systems/zlink/samples/tictactoe/server/api/ApiServerApplication.java");
        String authHandlerSource = sampleJavaSource(
            "TicTacToe",
            "Server/src/main/java",
            "systems/zlink/samples/tictactoe/server/api/handlers/AuthenticatePlayerHandler.java");
        String createGameHandlerSource = sampleJavaSource(
            "TicTacToe",
            "Server/src/main/java",
            "systems/zlink/samples/tictactoe/server/api/handlers/CreateGameHttpHandler.java");
        String settingsSource = sampleJavaSource(
            "TicTacToe",
            "Server/src/main/java",
            "systems/zlink/samples/tictactoe/server/configuration/ApiSettings.java")
            + sampleJavaSource(
                "TicTacToe",
                "Server/src/main/java",
                "systems/zlink/samples/tictactoe/server/configuration/PlaySettings.java")
            + sampleJavaSource(
                "TicTacToe",
                "Server/src/main/java",
                "systems/zlink/samples/tictactoe/server/configuration/SampleConfigPath.java");
        String playSource = sampleJavaSource(
            "TicTacToe",
            "Server/src/main/java",
            "systems/zlink/samples/tictactoe/server/play/PlayServer.java");
        String playHostFactorySource = sampleJavaSource(
            "TicTacToe",
            "Server/src/main/java",
            "systems/zlink/samples/tictactoe/server/play/PlayServerApplication.java");
        String playActorSource = sampleJavaSource(
            "TicTacToe",
            "Server/src/main/java",
            "systems/zlink/samples/tictactoe/server/play/infrastructure/zlink/actors/PlayActor.java");
        String entrySpotSource = sampleJavaSource(
            "TicTacToe",
            "Server/src/main/java",
            paths.playSpot("entryspot", "PlayEntrySpot"));
        String gameSpotSource = sampleJavaSource(
            "TicTacToe",
            "Server/src/main/java",
            paths.playSpot("tictactoegamespot", "TicTacToeGame"));
        String playSessionSource = sampleJavaSource(
            "TicTacToe",
            "Server/src/main/java",
            "systems/zlink/samples/tictactoe/server/play/infrastructure/zlink/sessions/PlaySession.java");

        assertTrue(apiProgramSource.contains("ApiServerApplication.run(SampleConfigPath.require(args))")
                && playProgramSource.contains("PlayServerApplication.run(SampleConfigPath.require(args))")
                && serverBuildSource.contains("playStartScripts")
                && !serverBuildSource.contains("implementation(sampleProject(\"Client\"))"),
            "TicTacToe Java must expose separate Api and Play executables that accept only a config path");
        assertFalse(apiProgramSource.contains("CountDownLatch")
                || playProgramSource.contains("ZLinkFramework.start"),
            "TicTacToe Java Server roles must rely on Spring lifecycle keep-alive instead of direct framework execution");
        assertTrue(apiSource.contains("ZLinkFrameworkConfigurer")
                && playSource.contains("ZLinkFrameworkConfigurer")
                && !apiSource.contains("options.codecs().use(")
                && !playSource.contains("options.codecs().use(")
                && !serverBuildSource.contains("zlink-framework-codec-msgpack")
                && apiHostFactorySource.contains("@SpringBootApplication")
                && apiHostFactorySource.contains("SpringApplicationBuilder")
                && apiHostFactorySource.contains(".web(WebApplicationType.SERVLET)")
                && apiHostFactorySource.contains("setKeepAlive(true)")
                && apiHostFactorySource.contains("ApiServer.configure(settings)")
                && playHostFactorySource.contains("@SpringBootApplication")
                && playHostFactorySource.contains("SpringApplicationBuilder")
                && playHostFactorySource.contains(".web(WebApplicationType.NONE)")
                && playHostFactorySource.contains("setKeepAlive(true)")
                && playHostFactorySource.contains("PlayServer.configure(settings)"),
            "TicTacToe direct Api and Play framework hosts must use the typed JSON default and expose HTTP create-game with shared settings");
        assertTrue(settingsSource.contains("@ConfigurationProperties(\"sample\")")
                && settingsSource.contains("--config")
                && settingsSource.contains("require(apiBindUrl, \"apiBindUrl\")")
                && !settingsSource.contains("--api-bind")
                && !settingsSource.contains("--api-url")
                && !settingsSource.contains("--api-channel-endpoint")
                && !settingsSource.contains("--play-channel-endpoint")
                && !settingsSource.contains("--route-endpoint")
                && !settingsSource.contains("--spot-endpoint")
                && !settingsSource.contains("--play-endpoint")
                && apiHostFactorySource.contains("SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME")
                && apiHostFactorySource.contains("SYSTEM_PROPERTIES_PROPERTY_SOURCE_NAME")
                && playHostFactorySource.contains("SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME")
                && playHostFactorySource.contains("SYSTEM_PROPERTIES_PROPERTY_SOURCE_NAME")
                && !settingsSource.contains("withEphemeralDefaults")
                && !settingsSource.contains("SamplePorts.reserve")
                && !settingsSource.contains("static SampleSettings current")
                && !settingsSource.contains("current()")
                && !settingsSource.contains("setCurrent"),
            "TicTacToe direct sample must expose .NET-style sample settings through Spring DI instead of fixed topology constants or global state");
        assertTrue(clientSource.contains("ZLinkHttpClient")
                && clientSource.contains("CreateGameHttpReq")
                && clientSource.contains("CreateGameHttpRes")
                && clientSource.contains(".post(\"/games\")")
                && !clientSource.contains(".requestToChannel("),
            "TicTacToe direct client must create games through the HTTP API path");
        String playAuthHandlerSource = sampleJavaSource(
            "TicTacToe",
            "Server/src/main/java",
            "systems/zlink/samples/tictactoe/server/play/infrastructure/zlink/sessions/handlers/AuthenticatePlaySessionHandler.java");
        assertTrue(playAuthHandlerSource.contains("new AuthenticatePlayerReq(request.accessToken())")
                && playAuthHandlerSource.contains(".submit(AuthenticatePlayerRes.class)")
                && authHandlerSource.contains("CompletionStage<AuthenticatePlayerRes> handle")
                && authHandlerSource.contains("AuthenticatePlayerReq request"),
            "TicTacToe direct Play session AuthenticatePlayer path must use typed request and response contracts");
        assertTrue(createGameHandlerSource.contains("@RestController")
                && createGameHandlerSource.contains("@PostMapping(\"/games\")")
                && createGameHandlerSource.contains("CompletionStage<CreateGameHttpRes> handle")
                && createGameHandlerSource.contains("CreateGameHttpReq")
                && createGameHandlerSource.contains("CreateGameHttpRes")
                && createGameHandlerSource.contains("ZLinkSpotManager")
                && createGameHandlerSource.contains(".create(\"tictactoe.game\")")
                && createGameHandlerSource.contains(".inMesh(SampleNames.SpotMesh)")
                && createGameHandlerSource.contains(".timeout(SampleNames.RequestTimeout)")
                && createGameHandlerSource.contains(".submit()")
                && !createGameHandlerSource.contains(".requestToChannel(")
                && !createGameHandlerSource.contains("HttpExchange")
                && !createGameHandlerSource.contains("HttpServer"),
            "TicTacToe HTTP create-game endpoint must create the game Spot through the object manager");
        assertTrue(clientSource.contains("ZLinkStreamConnectorFactory.create"),
            "TicTacToe Client role must use the public stream connector for play requests");
        assertTrue(clientSource.contains("new AuthenticateReq(options.xActorId())")
                && clientSource.contains("public final class TicTacToeClientScenario")
                && clientSource.contains("new JoinGameReq(game.roomId())")
                && clientSource.contains("new PlaceMarkReq(3)")
                && clientSource.contains("new PlaceMarkReq(4)")
                && clientSource.contains("new PlaceMarkReq(2)")
                && clientSource.contains("URI.create(endpoint)")
                && clientSource.contains("public void run(TicTacToeClientOptions options) throws Exception")
                && clientSource.contains(".request(new AuthenticateReq(options.xActorId()))")
                && clientSource.contains(".submit(AuthenticateRes.class)")
                && clientSource.contains(".request(new PlaceMarkReq(2))")
                && clientSource.contains(".submit(PlaceMarkRes.class)")
                && clientSource.contains("\"Won\".equals(hostWin.state().status())")
                && clientSource.contains("options.xActorId().equals(hostWin.state().winner())")
                && clientSource.contains("host.close().submit()")
                && clientSource.contains("guest.close().submit()")
                && !clientSource.contains("ZLinkMessagePackCodec")
                && !clientSource.contains("ZLinkMessagePackCodec.request")
                && !clientSource.contains(FORBIDDEN_TICTACTOE_RESULT)
                && !clientSource.contains("requestStep(")
                && !clientSource.contains("validateFinalState(")
                && !clientSource.contains("thenCompose(")
                && !clientSource.contains("game.gameId() + \"|\""),
            "TicTacToe stream client path must use connector member request contracts and assert the .NET winning scenario");
        assertTrue(clientSource.contains(".waitFor(PlayerJoinedNotify.class)")
                && clientSource.contains(".waitFor(GameStateNotify.class)")
                && clientSource.contains("ZLinkStreamDispatchMode.IMMEDIATE")
                && clientSource.contains(".where(PlayerJoinedNotify.class,")
                && clientSource.contains(".where(GameStateNotify.class,")
                && clientSource.contains(".submit(PlayerJoinedNotify.class)")
                && clientSource.contains(".submit(GameStateNotify.class)")
                && clientSource.contains("hostSawGuestJoin")
                && clientSource.contains("hostSawGameStart")
                && clientSource.contains("guestSawHostWin")
                && clientProgramSource.contains("new TicTacToeClientScenario().run(clientOptions)")
                && !clientProgramSource.contains("awaitSample(")
                && !clientSource.contains("ZLinkMessagePackCodec.on")
                && !clientSource.contains("ConcurrentLinkedQueue")
                && !clientSource.contains("stateNotifications")
                && !clientSource.contains("playerJoinedNotifications")
                && clientProgramSource.contains("tictactoe=completed")
                && !clientProgramSource.contains("writeTo(System.out)"),
            "TicTacToe direct client must use fluent wait and avoid returning a result DTO");
        assertFalse(clientSource.contains("systems.zlink.samples.tictactoe.server."),
            "TicTacToe Client role must not import server implementation");
        assertFalse(clientSource.contains("TicTacToeGameDirectory"),
            "TicTacToe Client role must not access server game storage directly");
        assertTrue(apiSource.contains(".addClientServerChannel(")
                && apiSource.contains("settings.apiChannelEndpoint()")
                && apiSource.contains("addRouteMesh(SampleNames.SpotMesh)")
                && apiSource.contains("mesh.objects().client()")
                && apiSource.contains("mesh.peerConnections().connect(endpoint)")
                && apiSource.contains("addHandlersFromPackageOf(ApiServer.class)")
                && apiSource.contains(".addHandlerGroup(\"api\")")
                && !apiSource.contains("addRequestHandler("),
            "TicTacToe Api role must discover handlers and create Spots through an Object Client RouteMesh");
        assertTrue(authHandlerSource.contains("implements ZLinkRequestHandler<")
                && authHandlerSource.contains("@ZLinkHandlerGroup(\"api\")")
                && !authHandlerSource.contains("@ZLinkRequest")
                && createGameHandlerSource.contains("@RequestBody")
                && createGameHandlerSource.contains("CreateGameHttpReq")
                && createGameHandlerSource.contains("CreateGameHttpRes"),
            "TicTacToe direct Api handlers must use framework discovery and HTTP create-game mapping");
        assertTrue(playSource.contains(".addRouteMesh(")
                && !playSource.contains(".addSpotMesh("),
            "TicTacToe direct sample must expose the Play Spot role through RouteMesh");
        assertTrue(playSource.contains("addHandlersFromPackageOf(PlayServer.class)")
                && !playSource.contains("addRequestHandler(")
                && playSource.contains("settings.apiChannelEndpoint()")
                && playSource.contains("settings.routeEndpoint()")
                && playSource.contains("settings.spotEndpoint()")
                && playSource.contains("settings.playEndpoint()")
                && !playSource.contains("addHandlerGroup("),
            "TicTacToe Play role must discover channel, Spot, actor, and session handlers automatically");
        String gameJoinReqSource = sampleJavaSource(
            "TicTacToe",
            "Shared/src/main/java",
            "systems/zlink/samples/tictactoe/shared/contracts/TicTacToeGameJoinReq.java");
        String gameJoinResSource = sampleJavaSource(
            "TicTacToe",
            "Shared/src/main/java",
            "systems/zlink/samples/tictactoe/shared/contracts/TicTacToeGameJoinRes.java");
        String playActorJoinHandlerSource = sampleJavaSource(
            "TicTacToe",
            "Server/src/main/java",
            paths.playSpotHandler("entryspot", "PlayActorJoinGameHandler"));
        String playPlaceMarkHandlerSource = sampleJavaSource(
            "TicTacToe",
            "Server/src/main/java",
            paths.playSpotHandler("tictactoegamespot", "PlayActorPlaceMarkHandler"));
        String gameCreatedHandlerSource = sampleJavaSource(
            "TicTacToe",
            "Server/src/main/java",
            paths.playSpotHandler("tictactoegamespot", "TicTacToeGameCreatedHandler"));
        String gameTimerHandlerSource = sampleJavaSource(
            "TicTacToe",
            "Server/src/main/java",
            paths.playSpotHandler("tictactoegamespot", "TicTacToeGameTimerHandler"));
        String matchDomainSource = sampleJavaSource(
            "TicTacToe",
            "Server/src/main/java",
            "systems/zlink/samples/tictactoe/server/play/domain/tictactoe/TicTacToeMatch.java");
        assertTrue(playAuthHandlerSource.contains("ZLinkTypedSessionPacketHandler<ZLinkSessionContext, AuthenticateReq>")
                && playAuthHandlerSource.contains("new AuthenticatePlayerReq(request.accessToken())")
                && playAuthHandlerSource.contains("context.actors().bind(requireActor(result))")
                && playSessionSource.contains("handlers.tryHandle(context, header, payload)")
                && playSessionSource.contains("requireActor(header.packetName()).relay(header, payload)")
                && !playSessionSource.contains("joinEntrySpot(")
                && !playSessionSource.contains("joinSpot(RoutingId.fromHex")
                && !playSessionSource.contains("split(\"\\\\|\")"),
            "TicTacToe Play stream session must authenticate through the Api role and relay actor packets");
        assertTrue(gameJoinReqSource.contains("String roomId")
                && gameJoinReqSource.contains("PlayerInfo player")
                && gameJoinResSource.contains("GameState state"),
            "TicTacToe direct sample must split client JoinGame contracts from Spot join contracts");
        assertTrue(gameSpotSource.contains("onActorJoin(")
                && gameSpotSource.contains("ZLinkMessage request")
                && gameSpotSource.contains("ZLinkSpotActorJoinResult.accept")
                && gameSpotSource.contains("TicTacToeGameJoinReq.class")
                && gameSpotSource.contains("TicTacToeGameJoinRes")
                && playActorJoinHandlerSource.contains("new TicTacToeGameJoinReq")
                && playActorJoinHandlerSource.contains("PlayEntrySpot entrySpot")
                && playActorJoinHandlerSource.contains("@ZLinkSpotActorSend")
                && playActorJoinHandlerSource.contains("ZLinkMessageContext context")
                && playActorJoinHandlerSource.contains("trackDeferredJoin")
                && playActorJoinHandlerSource.contains(".defer()")
                && playActorSource.contains("onJoinCompleted(")
                && playActorSource.contains("new JoinGameRes")
                && playPlaceMarkHandlerSource.contains("TicTacToeGame spot")
                && playPlaceMarkHandlerSource.contains("ZLinkMessageContext context")
                && playPlaceMarkHandlerSource.contains("PlaceMarkReq request"),
            "TicTacToe Play actor join must defer the framework operation and publish its completion while game packets stay typed");
        assertTrue(gameSpotSource.contains("actor.joinGame")
                && gameSpotSource.contains("boundSession()")
                && gameSpotSource.contains(".send(new GameStateNotify")
                && gameSpotSource.contains(".send(message)"),
            "TicTacToe game Spot must own joined-game state transitions and typed bound-session notifications");
        assertTrue(gameSpotSource.contains("onInitialize()")
                && gameSpotSource.contains("context.addTimer(")
                && gameSpotSource.contains("TicTacToeGameTimerHandler.class")
                && gameSpotSource.contains("onClosing()")
                && gameSpotSource.contains("gameTick.cancel()")
                && matchDomainSource.contains("TurnTimedOut")
                && matchDomainSource.contains("resetTurnDeadline(")
                && gameSpotSource.contains("tick()")
                && gameSpotSource.contains("onCreate(ZLinkMessage request)")
                && gameSpotSource.contains("markCreated(ZLinkMessage request)")
                && gameSpotSource.contains("ensureCreated()")
                && gameCreatedHandlerSource.contains("handle(")
                && gameCreatedHandlerSource.contains("ZLinkMessage request")
                && gameCreatedHandlerSource.contains("ZLinkSpotCreateResponse.accept()")
                && gameCreatedHandlerSource.contains("game.markCreated(request)")
                && gameTimerHandlerSource.contains("implements ZLinkSpotTimerHandler<TicTacToeGame>")
                && gameTimerHandlerSource.contains("spot.tick()"),
            "TicTacToe game Spot must mirror the .NET lifecycle, timer, and turn-timeout API usage");
        assertTrue(gameSpotSource.contains("onJoinedActor(")
                && gameSpotSource.contains("onLeaveActor(")
                && entrySpotSource.contains("onCreateActor(")
                && entrySpotSource.contains("ZLinkMessage createRequest")
                && entrySpotSource.contains("ZLinkActorCreateResponse.accept")
                && !gameSpotSource.contains("ZLinkSpotActorChange" + "Result"),
            "TicTacToe EntrySpot and GameSpot lifecycle must use Actor creation and member callbacks without change-result arguments");
        assertTrue(playActorJoinHandlerSource.contains("request.roomId()"),
            "TicTacToe join handler must store the requested room id");
        assertTrue(playPlaceMarkHandlerSource.contains("actor.requireJoinedGame()"),
            "TicTacToe place handler must require actor join state");
        assertTrue(playActorSource.contains("joinedRoomId"),
            "TicTacToe PlayActor must own joined room state");
        assertTrue(playActorSource.contains("joinGame"),
            "TicTacToe PlayActor must expose joinGame state transition");
        assertTrue(playActorSource.contains("requireJoinedGame"),
            "TicTacToe PlayActor must validate joined game state");
        assertTrue(entrySpotSource.contains("public PlayEntrySpot(")
                && entrySpotSource.contains("ZLinkEntrySpotContext context")
                && !entrySpotSource.contains("public PlayEntrySpot()")
                && gameSpotSource.contains("ZLinkSpotContext context")
                && gameSpotSource.contains("TicTacToeGameCreatedHandler createdHandler")
                && !gameSpotSource.contains("public TicTacToeGame()")
                && !gameSpotSource.contains("SampleSpotContext")
                && !gameSpotSource.contains("CompletedSpotOutbound")
                && !gameSpotSource.contains("TicTacToeGameDirectory"),
            "TicTacToe Spot instances must be created by the framework runtime, not sample-owned fallback contexts");
        assertTrue(playSource.contains(".addStreamNode("),
            "TicTacToe direct sample must register the STREAM entry point");
        assertFalse(apiProgramSource.contains("CreateGameHandler")
                || playProgramSource.contains("CreateGameHandler"),
            "TicTacToe role Program must not collapse Play handler wiring into the entry point");
    }

    @Test
    void ticTacToeKotlinSampleMirrorsJavaRoleLayout() throws IOException {
        SampleSourcePaths paths = kotlinSamplePaths("tictactoe");
        assertNoSampleSourcesUnder("kotlin", "TicTacToe", "src/main/kotlin",
            List.of(
                "systems/zlink/samples/kotlin/tictactoe/client",
                "systems/zlink/samples/kotlin/tictactoe/server",
                "systems/zlink/samples/kotlin/tictactoe/shared"));
        assertSampleFilesExist("kotlin", "TicTacToe", "Client/src/main/kotlin", List.of(
            "systems/zlink/samples/kotlin/tictactoe/client/TicTacToeClientArguments.kt",
            "systems/zlink/samples/kotlin/tictactoe/client/TicTacToeClientScenario.kt",
            "systems/zlink/samples/kotlin/tictactoe/client/TicTacToeClientOptions.kt",
            "systems/zlink/samples/kotlin/tictactoe/client/TicTacToeSampleDefaults.kt"));
        assertTrue(sampleFileContains("kotlin", "TicTacToe", "Client",
                "README.md", "Tic Tac Toe Client"),
            "Kotlin TicTacToe Client role must include a standalone README");
        assertSampleFilesExist("kotlin", "TicTacToe", "Server/src/main/kotlin", List.of(
            "systems/zlink/samples/kotlin/tictactoe/server/api/ApiServer.kt",
            "systems/zlink/samples/kotlin/tictactoe/server/api/ApiServerApplication.kt",
            "systems/zlink/samples/kotlin/tictactoe/server/api/handlers/AuthenticatePlayerHandler.kt",
            "systems/zlink/samples/kotlin/tictactoe/server/api/handlers/CreateGameHttpHandler.kt",
            "systems/zlink/samples/kotlin/tictactoe/server/configuration/SampleLogging.kt",
            "systems/zlink/samples/kotlin/tictactoe/server/configuration/SampleNames.kt",
            "systems/zlink/samples/kotlin/tictactoe/server/configuration/SampleSettings.kt",
            "systems/zlink/samples/kotlin/tictactoe/server/play/PlayServer.kt",
            "systems/zlink/samples/kotlin/tictactoe/server/play/PlayServerApplication.kt",
            "systems/zlink/samples/kotlin/tictactoe/server/play/infrastructure/zlink/actors/PlayActor.kt",
            "systems/zlink/samples/kotlin/tictactoe/server/play/infrastructure/zlink/actors/PlayActorFactory.kt",
            paths.playSpot("entryspot", "PlayEntrySpot"),
            paths.playSpot("tictactoegamespot", "TicTacToeGame"),
            paths.playSpotHandler("tictactoegamespot", "PlayActorPlaceMarkHandler"),
            paths.playSpotHandler("tictactoegamespot", "TicTacToeGameTimerHandler"),
            "systems/zlink/samples/kotlin/tictactoe/server/play/infrastructure/zlink/sessions/PlaySession.kt",
            "systems/zlink/samples/kotlin/tictactoe/server/play/infrastructure/zlink/sessions/handlers/AuthenticatePlaySessionHandler.kt"));
        assertSampleFilesExist("kotlin", "TicTacToe", "Shared/src/main/kotlin", List.of(
            "systems/zlink/samples/kotlin/tictactoe/shared/contracts/Contracts.kt"));
        assertTrue(sampleFileContains("kotlin", "TicTacToe", "Client/src/main/kotlin",
                "systems/zlink/samples/kotlin/tictactoe/client/Program.kt", "TicTacToeClientArguments.parse"),
            "Kotlin TicTacToe Client role Program must live in the Client project folder");
        String apiProgramSource = sampleKotlinSource(
            "TicTacToe",
            "Server/src/main/kotlin",
            "systems/zlink/samples/kotlin/tictactoe/server/api/ApiProgram.kt");
        String playProgramSource = sampleKotlinSource(
            "TicTacToe",
            "Server/src/main/kotlin",
            "systems/zlink/samples/kotlin/tictactoe/server/play/PlayProgram.kt");
        String serverBuildSource = sampleFile(
            "kotlin",
            "TicTacToe",
            "Server",
            "build.gradle.kts");
        String clientSource = sampleKotlinSource(
            "TicTacToe",
            "Client/src/main/kotlin",
            "systems/zlink/samples/kotlin/tictactoe/client/TicTacToeClientScenario.kt");
        String clientProgramSource = sampleKotlinSource(
            "TicTacToe",
            "Client/src/main/kotlin",
            "systems/zlink/samples/kotlin/tictactoe/client/Program.kt");
        String apiSource = sampleKotlinSource(
            "TicTacToe",
            "Server/src/main/kotlin",
            "systems/zlink/samples/kotlin/tictactoe/server/api/ApiServer.kt");
        String apiHostFactorySource = sampleKotlinSource(
            "TicTacToe",
            "Server/src/main/kotlin",
            "systems/zlink/samples/kotlin/tictactoe/server/api/ApiServerApplication.kt");
        String authHandlerSource = sampleKotlinSource(
            "TicTacToe",
            "Server/src/main/kotlin",
            "systems/zlink/samples/kotlin/tictactoe/server/api/handlers/AuthenticatePlayerHandler.kt");
        String createGameHandlerSource = sampleKotlinSource(
            "TicTacToe",
            "Server/src/main/kotlin",
            "systems/zlink/samples/kotlin/tictactoe/server/api/handlers/CreateGameHttpHandler.kt");
        String settingsSource = sampleKotlinSource(
            "TicTacToe",
            "Server/src/main/kotlin",
            "systems/zlink/samples/kotlin/tictactoe/server/configuration/SampleSettings.kt");
        String playSource = sampleKotlinSource(
            "TicTacToe",
            "Server/src/main/kotlin",
            "systems/zlink/samples/kotlin/tictactoe/server/play/PlayServer.kt");
        String playHostFactorySource = sampleKotlinSource(
            "TicTacToe",
            "Server/src/main/kotlin",
            "systems/zlink/samples/kotlin/tictactoe/server/play/PlayServerApplication.kt");
        String playActorSource = sampleKotlinSource(
            "TicTacToe",
            "Server/src/main/kotlin",
            "systems/zlink/samples/kotlin/tictactoe/server/play/infrastructure/zlink/actors/PlayActor.kt");
        String entrySpotSource = sampleKotlinSource(
            "TicTacToe",
            "Server/src/main/kotlin",
            paths.playSpot("entryspot", "PlayEntrySpot"));
        String gameSpotSource = sampleKotlinSource(
            "TicTacToe",
            "Server/src/main/kotlin",
            paths.playSpot("tictactoegamespot", "TicTacToeGame"));
        String gameMatchSource = sampleKotlinSource(
            "TicTacToe",
            "Server/src/main/kotlin",
            "systems/zlink/samples/kotlin/tictactoe/server/play/domain/tictactoe/TicTacToeMatch.kt");
        String playSessionSource = sampleKotlinSource(
            "TicTacToe",
            "Server/src/main/kotlin",
            "systems/zlink/samples/kotlin/tictactoe/server/play/infrastructure/zlink/sessions/PlaySession.kt");
        String playAuthHandlerSource = sampleKotlinSource(
            "TicTacToe",
            "Server/src/main/kotlin",
            "systems/zlink/samples/kotlin/tictactoe/server/play/infrastructure/zlink/sessions/handlers/AuthenticatePlaySessionHandler.kt");

        assertTrue(apiProgramSource.contains("ApiServerApplication.run(SampleSettings.configPath(args))")
                && playProgramSource.contains("PlayServerApplication.run(SampleSettings.configPath(args))")
                && serverBuildSource.contains("playStartScripts")
                && !serverBuildSource.contains("implementation(sampleProject(\"Client\"))"),
            "Kotlin TicTacToe must expose separate Api and Play executables that accept only a config path");
        assertFalse(apiProgramSource.contains("CountDownLatch")
                || playProgramSource.contains("ZLinkFramework.start"),
            "Kotlin TicTacToe Server roles must rely on Spring lifecycle keep-alive instead of direct framework execution");
        assertTrue(apiSource.contains("ZLinkFrameworkConfigurer")
                && apiSource.contains("addHandlersFromPackageOf(ApiServer::class.java)")
                && !apiSource.contains("ZLinkMessagePackCodec")
                && playSource.contains("ZLinkFrameworkConfigurer")
                && playSource.contains("addHandlersFromPackageOf(PlayServer::class.java)")
                && !playSource.contains("ZLinkMessagePackCodec")
                && !serverBuildSource.contains("zlink-framework-codec-msgpack")
                && apiHostFactorySource.contains("@SpringBootApplication")
                && apiHostFactorySource.contains("SpringApplicationBuilder")
                && apiHostFactorySource.contains(".web(WebApplicationType.SERVLET)")
                && apiHostFactorySource.contains("setKeepAlive(true)")
                && apiHostFactorySource.contains("ApiServer.configure(settings)")
                && playHostFactorySource.contains("@SpringBootApplication")
                && playHostFactorySource.contains("SpringApplicationBuilder")
                && playHostFactorySource.contains(".web(WebApplicationType.NONE)")
                && playHostFactorySource.contains("setKeepAlive(true)")
                && playHostFactorySource.contains("PlayServer.configure(settings)"),
            "Kotlin TicTacToe Api and Play hosts must use automatic handler discovery and the default JSON codec");
        assertTrue(settingsSource.contains("@ConfigurationProperties(\"sample\")")
                && settingsSource.contains("--config")
                && settingsSource.contains("sample.apiBindUrl")
                && !settingsSource.contains("--api-bind")
                && !settingsSource.contains("--api-url")
                && !settingsSource.contains("--api-channel-endpoint")
                && !settingsSource.contains("--play-channel-endpoint")
                && !settingsSource.contains("--route-endpoint")
                && !settingsSource.contains("--spot-endpoint")
                && !settingsSource.contains("--play-endpoint")
                && apiHostFactorySource.contains("SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME")
                && apiHostFactorySource.contains("SYSTEM_PROPERTIES_PROPERTY_SOURCE_NAME")
                && playHostFactorySource.contains("SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME")
                && playHostFactorySource.contains("SYSTEM_PROPERTIES_PROPERTY_SOURCE_NAME")
                && !settingsSource.contains("withEphemeralDefaults")
                && !settingsSource.contains("SamplePorts.reserve")
                && !settingsSource.contains("currentValue")
                && !settingsSource.contains("fun current")
                && !settingsSource.contains("setCurrent"),
            "Kotlin TicTacToe direct sample must expose .NET-style sample settings through Spring DI instead of fixed topology constants or global state");
        assertTrue(clientSource.contains("zlinkHttpClient")
                && clientSource.contains("CreateGameHttpReq")
                && clientSource.contains("CreateGameHttpRes")
                && clientSource.contains(".post(\"/games\")")
                && !clientSource.contains(".requestToChannel("),
            "Kotlin TicTacToe direct client must create games through the HTTP API path");
        assertTrue(playAuthHandlerSource.contains("AuthenticatePlayerReq(request.accessToken)")
                && playAuthHandlerSource.contains(".submit(AuthenticatePlayerRes::class.java)")
                && authHandlerSource.contains("ZLinkSuspendingRequestHandler<AuthenticatePlayerReq, AuthenticatePlayerRes>")
                && authHandlerSource.contains("override suspend fun handle(")
                && authHandlerSource.contains("): AuthenticatePlayerRes"),
            "Kotlin TicTacToe direct Play session AuthenticatePlayer path must use typed request and response contracts");
        assertTrue(createGameHandlerSource.contains("@RestController")
                && createGameHandlerSource.contains("@PostMapping(\"/games\")")
                && createGameHandlerSource.contains("suspend fun handle(@RequestBody request: CreateGameHttpReq): CreateGameHttpRes")
                && createGameHandlerSource.contains("CreateGameHttpReq")
                && createGameHandlerSource.contains("CreateGameHttpRes")
                && createGameHandlerSource.contains("ZLinkSpotManager")
                && createGameHandlerSource.contains(".create(\"tictactoe.game\")")
                && createGameHandlerSource.contains(".inMesh(SampleNames.SpotMesh)")
                && createGameHandlerSource.contains(".timeout(SampleNames.RequestTimeout)")
                && createGameHandlerSource.contains(".submit()")
                && !createGameHandlerSource.contains(".requestToChannel(")
                && !createGameHandlerSource.contains("HttpExchange")
                && !createGameHandlerSource.contains("HttpServer"),
            "Kotlin TicTacToe HTTP create-game endpoint must create the game Spot through the object manager");
        assertTrue(clientSource.contains("ZLinkStreamConnectorFactory.create"),
            "Kotlin TicTacToe Client role must use the public stream connector for play requests");
        assertTrue(clientSource.contains("ZLinkKotlinStreamConnector")
                && clientSource.contains(".kotlin()")
                && clientSource.contains("hostStream.close().await()")
                && clientSource.contains("guestStream.close().await()")
                && !clientSource.contains(".submit(AuthenticateRes::class.java)")
                && !clientSource.contains(".submit(JoinGameRes::class.java)")
                && !clientSource.contains(".submit(PlaceMarkRes::class.java)"),
            "Kotlin TicTacToe client scenario must use coroutine connector wrappers for request calls");
        assertTrue(clientSource.contains(".request(AuthenticateReq(options.xActorId)).awaitReply<AuthenticateRes>()")
                && clientSource.contains("AuthenticateReq(options.xActorId)")
                && clientSource.contains("JoinGameReq(game.roomId)")
                && clientSource.contains("PlaceMarkReq(3)")
                && clientSource.contains("PlaceMarkReq(4)")
                && clientSource.contains("PlaceMarkReq(2)")
                && clientSource.contains("URI.create(endpoint)")
                && clientSource.contains("ensure(hostWin.state.status == \"Won\")")
                && clientSource.contains("ensure(hostWin.state.winner == options.xActorId)")
                && clientSource.contains("ZLinkStreamJson.codec()")
                && !clientSource.contains("ZLinkMessagePackCodec.request")
                && !clientSource.contains(FORBIDDEN_TICTACTOE_RESULT)
                && !clientSource.contains("game.gameId"),
            "Kotlin TicTacToe stream client path must use connector member request contracts and assert the .NET winning scenario");
        assertTrue(clientSource.contains("hostStream.waitFor<PlayerJoinedNotify>()")
                && clientSource.contains("class TicTacToeClientScenario")
                && clientSource.contains("hostStream.waitFor<GameStateNotify>()")
                && clientSource.contains("guestStream.waitFor<GameStateNotify>()")
                && clientSource.contains("ZLinkStreamDispatchMode.IMMEDIATE")
                && clientSource.contains("hostSawGuestJoin")
                && clientSource.contains("hostSawGameStart")
                && clientSource.contains("guestSawHostWin")
                && clientSource.contains(".where { message -> message.payload().state.lastMoveCell == 0 }")
                && clientSource.contains(".where { message -> message.payload().state.status == \"Won\" }")
                && !clientSource.contains("ZLinkMessagePackCodec.on")
                && !clientSource.contains("ConcurrentLinkedQueue")
                && !clientSource.contains("stateNotifications")
                && !clientSource.contains("playerJoinedNotifications")
                && clientProgramSource.contains("TicTacToeClientScenario().run(clientOptions)")
                && !clientProgramSource.contains("writeTo(System.out)"),
            "Kotlin TicTacToe direct client must subscribe to typed stream notifications without returning a result DTO");
        assertFalse(clientSource.contains("systems.zlink.samples.kotlin.tictactoe.server."),
            "Kotlin TicTacToe Client role must not import server implementation");
        assertFalse(clientSource.contains("TicTacToeGameDirectory"),
            "Kotlin TicTacToe Client role must not access server game storage directly");
        assertTrue(apiSource.contains(".addClientServerChannel(")
                && apiSource.contains("settings.apiChannelEndpoint")
                && apiSource.contains("addRouteMesh(SampleNames.SpotMesh)")
                && apiSource.contains("mesh.objects().client()")
                && apiSource.contains("mesh.peerConnections().connect(endpoint)")
                && apiSource.contains("addHandlersFromPackageOf(ApiServer::class.java)")
                && apiSource.contains("addHandlerGroup(\"api\")")
                && !apiSource.contains("addRequestHandler("),
            "Kotlin TicTacToe Api must discover handlers and create Spots through an Object Client RouteMesh");
        assertTrue(authHandlerSource.contains("@ZLinkHandlerGroup(\"api\")")
                && !authHandlerSource.contains("@ZLinkRequest(packetName = \"AuthenticatePlayerReq\")")
                && createGameHandlerSource.contains("@RequestBody")
                && createGameHandlerSource.contains("CreateGameHttpReq")
                && createGameHandlerSource.contains("CreateGameHttpRes"),
            "Kotlin TicTacToe direct Api handlers must use annotation-based auth and HTTP create-game mapping");
        assertTrue(playSource.contains(".addRouteMesh(")
                && !playSource.contains(".addSpotMesh("),
            "Kotlin TicTacToe direct sample must expose the Play Spot role through RouteMesh");
        assertTrue(playSource.contains("addHandlersFromPackageOf(PlayServer::class.java)")
                && !playSource.contains("addRequestHandler(")
                && playSource.contains("settings.apiChannelEndpoint")
                && playSource.contains("settings.routeEndpoint")
                && playSource.contains("settings.playEndpoint")
                && playSource.contains("node.listen(routeEndpoint)")
                && playSource.contains("node.channelName(SampleNames.PlayNode)")
                && playSource.contains("node.peerConnections().connect(")
                && !playSource.contains("addHandlerGroup(SampleNames.PlayHandlerGroup)"),
            "Kotlin TicTacToe Play role must discover Spot, actor, and session handlers automatically");
        assertTrue(clientSource.contains("JoinGameReq(game.roomId)")
                && !clientSource.contains("TicTacToeGameJoinReq"),
            "Kotlin TicTacToe client must use client-facing JoinGame contracts only");
        String playActorJoinHandlerSource = sampleKotlinSource(
            "TicTacToe",
            "Server/src/main/kotlin",
            paths.playSpotHandler("entryspot", "PlayActorJoinGameHandler"));
        String playPlaceMarkHandlerSource = sampleKotlinSource(
            "TicTacToe",
            "Server/src/main/kotlin",
            paths.playSpotHandler("tictactoegamespot", "PlayActorPlaceMarkHandler"));
        String gameCreatedHandlerSource = sampleKotlinSource(
            "TicTacToe",
            "Server/src/main/kotlin",
            paths.playSpotHandler("tictactoegamespot", "TicTacToeGameCreatedHandler"));
        String gameTimerHandlerSource = sampleKotlinSource(
            "TicTacToe",
            "Server/src/main/kotlin",
            paths.playSpotHandler("tictactoegamespot", "TicTacToeGameTimerHandler"));
        assertTrue(playAuthHandlerSource.contains("ZLinkSuspendingTypedSessionPacketHandler<ZLinkSessionContext, AuthenticateReq>")
                && playAuthHandlerSource.contains("AuthenticatePlayerReq(request.accessToken)")
                && playAuthHandlerSource.contains("context.actors().bind(requireActor(playActor)).await()")
                && playSessionSource.contains("handlers.tryHandle(context, header, payload)")
                && playSessionSource.contains("requireActor(header.packetName()).relay(header, payload)")
                && !playSessionSource.contains("joinEntrySpot(")
                && !playSessionSource.contains("joinSpot(RoutingId.fromHex")
                && !playSessionSource.contains("split(\"|\")"),
            "Kotlin TicTacToe Play stream session must authenticate through the Api role and relay actor packets");
        assertTrue(gameSpotSource.contains("override suspend fun onActorJoinSuspending(")
                && gameSpotSource.contains("request: ZLinkMessage")
                && gameSpotSource.contains("ZLinkSpotActorJoinResult.accept")
                && gameSpotSource.contains("TicTacToeGameJoinReq::class.java")
                && gameSpotSource.contains("TicTacToeGameJoinRes")
                && playActorJoinHandlerSource.contains("TicTacToeGameJoinReq")
                && playActorJoinHandlerSource.contains("entrySpot: PlayEntrySpot")
                && playActorJoinHandlerSource.contains("@ZLinkSpotActorSend")
                && playActorJoinHandlerSource.contains("context: ZLinkMessageContext")
                && playActorJoinHandlerSource.contains("trackDeferredJoin")
                && playActorJoinHandlerSource.contains(".defer()")
                && playActorSource.contains("override fun onJoinCompleted(")
                && playActorSource.contains("send(JoinGameRes(reply.state))")
                && playPlaceMarkHandlerSource.contains("spot: TicTacToeGame")
                && playPlaceMarkHandlerSource.contains("context: ZLinkMessageContext")
                && playPlaceMarkHandlerSource.contains("request: PlaceMarkReq")
                && !playPlaceMarkHandlerSource.contains("CancellationToken"),
            "Kotlin TicTacToe Play actor join must defer the framework operation and publish its completion while game packets stay typed");
        assertTrue(gameSpotSource.contains("actor.joinGame")
                && gameSpotSource.contains("boundSession()")
                && gameSpotSource.contains(".send(GameStateNotify")
                && gameSpotSource.contains(".send(message)"),
            "Kotlin TicTacToe game Spot must own joined-game state transitions and typed bound-session notifications");
        assertTrue(gameSpotSource.contains("override suspend fun onInitializeSuspending()")
                && gameSpotSource.contains("context.addTimer(")
                && gameSpotSource.contains("TicTacToeGameTimerHandler::class.java")
                && gameSpotSource.contains("override suspend fun onClosingSuspending(context: ZLinkSpotClosingContext)")
                && gameSpotSource.contains("gameTick?.cancel()")
                && gameMatchSource.contains("TurnTimedOut")
                && gameMatchSource.contains("resetTurnDeadline")
                && gameSpotSource.contains("suspend fun tick()")
                && gameSpotSource.contains("override suspend fun onCreateSuspending(request: ZLinkMessage)")
                && gameSpotSource.contains("fun markCreated(request: ZLinkMessage)")
                && gameSpotSource.contains("ensureCreated()")
                && gameCreatedHandlerSource.contains("fun handle(")
                && gameCreatedHandlerSource.contains("request: ZLinkMessage")
                && gameCreatedHandlerSource.contains("game.markCreated(request)")
                && gameTimerHandlerSource.contains("ZLinkSuspendingSpotTimerHandler<TicTacToeGame>")
                && gameTimerHandlerSource.contains("spot.tick()"),
            "Kotlin TicTacToe game Spot must mirror the .NET lifecycle, timer, and turn-timeout API usage");
        assertTrue(gameSpotSource.contains("override suspend fun onJoinedActorSuspending(")
                && gameSpotSource.contains("override suspend fun onLeaveActorSuspending(")
                && entrySpotSource.contains("override suspend fun onCreateActorSuspending(")
                && entrySpotSource.contains("createRequest: ZLinkMessage")
                && entrySpotSource.contains("ZLinkActorCreateResponse.accept")
                && !gameSpotSource.contains("ZLinkSpotActorChange" + "Result"),
            "Kotlin TicTacToe EntrySpot and GameSpot lifecycle must use Actor creation and member callbacks without change-result arguments");
        assertTrue(playActorJoinHandlerSource.contains("request.roomId"),
            "Kotlin TicTacToe join handler must store the requested room id");
        assertTrue(playPlaceMarkHandlerSource.contains("actor.requireJoinedGame()"),
            "Kotlin TicTacToe place handler must require actor join state");
        assertTrue(playActorSource.contains("joinedRoomId"),
            "Kotlin TicTacToe PlayActor must own joined room state");
        assertTrue(playActorSource.contains("fun joinGame"),
            "Kotlin TicTacToe PlayActor must expose joinGame state transition");
        assertTrue(playActorSource.contains("fun requireJoinedGame"),
            "Kotlin TicTacToe PlayActor must validate joined game state");
        assertTrue(entrySpotSource.contains("override val context: ZLinkEntrySpotContext")
                && !entrySpotSource.contains("ZLinkEntrySpotContext? = null")
                && gameSpotSource.contains("override val context: ZLinkSpotContext")
                && gameSpotSource.contains("private val createdHandler: TicTacToeGameCreatedHandler")
                && !gameSpotSource.contains("constructor()")
                && !gameSpotSource.contains("SampleSpotContext")
                && !gameSpotSource.contains("CompletedSpotOutbound")
                && !gameSpotSource.contains("TicTacToeGameDirectory"),
            "Kotlin TicTacToe Spot instances must be created by the framework runtime, not sample-owned fallback contexts");
        assertTrue(playSource.contains(".addStreamNode("),
            "Kotlin TicTacToe direct sample must register the STREAM entry point");
        assertFalse(apiProgramSource.contains("CreateGameHandler")
                || playProgramSource.contains("CreateGameHandler"),
            "Kotlin TicTacToe role Program must not collapse Play handler wiring into the entry point");
    }

    @Test
    void bingoMirrorsFourClientMatchingTimerAndBoundPushGate() throws IOException {
        SampleSourcePaths paths = javaSamplePaths("bingo");
        assertNoSampleSourcesUnder("java", "Bingo", "src/main/java");
        assertSampleFilesExist("java", "Bingo", "Client/src/main/java", List.of(
            "systems/zlink/samples/bingo/client/Program.java",
            "systems/zlink/samples/bingo/client/BingoClientScenario.java"));
        assertSampleFilesExist("java", "Bingo", "Server/Api/src/main/java", List.of(
            "systems/zlink/samples/bingo/server/api/Program.java",
            "systems/zlink/samples/bingo/server/api/ApiServerApplication.java",
            "systems/zlink/samples/bingo/server/api/handlers/AuthenticatePlayerHandler.java",
            "systems/zlink/samples/bingo/server/api/handlers/MatchBingoHandler.java"));
        assertSampleFilesExist("java", "Bingo", "Server/Play/src/main/java", List.of(
            "systems/zlink/samples/bingo/server/play/Program.java",
            "systems/zlink/samples/bingo/server/play/PlayServerApplication.java",
            "systems/zlink/samples/bingo/server/play/infrastructure/zlink/actors/PlayerActor.java",
            "systems/zlink/samples/bingo/server/play/infrastructure/zlink/actors/PlayerActorFactory.java",
            "systems/zlink/samples/bingo/server/play/domain/bingo/BingoCard.java",
            "systems/zlink/samples/bingo/server/play/domain/bingo/BingoGame.java",
            "systems/zlink/samples/bingo/server/play/domain/bingo/BingoRoomGame.java",
            "systems/zlink/samples/bingo/server/play/domain/bingo/BingoRoomModels.java",
            paths.playSpot("bingoroomspot", "BingoRoomSpot"),
            paths.playSpotHandler("bingoroomspot", "BingoRoomSettingsInitializer"),
            paths.playSpotHandler("bingoroomspot", "BingoRoomTimerHandler"),
            paths.playSpotHandler("bingoroomspot", "SubmitBingoCardHandler"),
            paths.playSpot("bingoroomspot", "BingoRoomRelocationAdapter"),
            paths.playSpot("entryspot", "BingoEntrySpot"),
            paths.playSpotHandler("entryspot", "MatchBingoActorHandler")));
        assertSampleFilesExist("java", "Bingo", "Server/Matchmaking/src/main/java", List.of(
            "systems/zlink/samples/bingo/server/matchmaking/MatchmakingServerApplication.java",
            "systems/zlink/samples/bingo/server/matchmaking/BingoMatchmaker.java",
            "systems/zlink/samples/bingo/server/matchmaking/BingoMatchmakerIdleTimerHandler.java",
            "systems/zlink/samples/bingo/server/matchmaking/ReserveBingoRoomHandler.java"));
        assertSampleFilesExist("java", "Bingo", "Server/Session/src/main/java", List.of(
            "systems/zlink/samples/bingo/server/session/Program.java",
            "systems/zlink/samples/bingo/server/session/SessionServerApplication.java",
            "systems/zlink/samples/bingo/server/session/sessions/BingoSession.java",
            "systems/zlink/samples/bingo/server/session/sessions/handlers/AuthenticateSessionHandler.java"));
        assertSampleFilesExist("java", "Bingo", "Server/Configuration/src/main/java", List.of(
            "systems/zlink/samples/bingo/server/configuration/SampleLocationStore.java",
            "systems/zlink/samples/bingo/server/configuration/SampleNames.java",
            "systems/zlink/samples/bingo/server/configuration/SampleTopology.java",
            "systems/zlink/samples/bingo/server/configuration/SampleTimings.java"));
        assertSampleFilesExist("java", "Bingo", "Client/src/main/java", List.of(
            "systems/zlink/samples/bingo/client/configuration/SampleNames.java",
            "systems/zlink/samples/bingo/client/configuration/SampleTopology.java",
            "systems/zlink/samples/bingo/client/configuration/SampleTimings.java"));
        assertSampleFilesExist("java", "Bingo", "Shared/src/main/java", List.of(
            "systems/zlink/samples/bingo/shared/contracts/BingoMessages.java"));
        assertSampleFilesExist("java", "Bingo", "Shared/src/main/proto", List.of(
            "bingo_messages.proto"));

        String rootBuildSource = sampleFile(
            "java",
            "Bingo",
            "",
            "build.gradle.kts");
        String sharedBuildSource = sampleFile(
            "java",
            "Bingo",
            "Shared",
            "build.gradle.kts");
        String sharedContractsSource = sampleJavaSource(
            "Bingo",
            "Shared/src/main/java",
            "systems/zlink/samples/bingo/shared/contracts/BingoMessages.java");
        String sharedProtoSource = sampleFile(
            "java",
            "Bingo",
            "Shared/src/main/proto",
            "bingo_messages.proto");
        String clientProgramSource = sampleJavaSource(
            "Bingo",
            "Client/src/main/java",
            "systems/zlink/samples/bingo/client/Program.java");
        String clientAppSource = sampleJavaSource(
            "Bingo",
            "Client/src/main/java",
            "systems/zlink/samples/bingo/client/BingoClientScenario.java");
        String roomSource = sampleJavaSource(
            "Bingo",
            "Server/Play/src/main/java",
            paths.playSpot("bingoroomspot", "BingoRoomSpot"));
        String apiHostSource = sampleJavaSource(
            "Bingo",
            "Server/Api/src/main/java",
            "systems/zlink/samples/bingo/server/api/ApiServerApplication.java");
        String playHostSource = sampleJavaSource(
            "Bingo",
            "Server/Play/src/main/java",
            "systems/zlink/samples/bingo/server/play/PlayServerApplication.java");
        String sessionHostSource = sampleJavaSource(
            "Bingo",
            "Server/Session/src/main/java",
            "systems/zlink/samples/bingo/server/session/SessionServerApplication.java");
        String locationStoreSource = sampleJavaSource(
            "Bingo",
            "Server/Configuration/src/main/java",
            "systems/zlink/samples/bingo/server/configuration/SampleLocationStore.java");
        String apiProgramSource = sampleJavaSource(
            "Bingo",
            "Server/Api/src/main/java",
            "systems/zlink/samples/bingo/server/api/Program.java");
        String playProgramSource = sampleJavaSource(
            "Bingo",
            "Server/Play/src/main/java",
            "systems/zlink/samples/bingo/server/play/Program.java");
        String entrySpotSource = sampleJavaSource(
            "Bingo",
            "Server/Play/src/main/java",
            paths.playSpot("entryspot", "BingoEntrySpot"));
        String sessionProgramSource = sampleJavaSource(
            "Bingo",
            "Server/Session/src/main/java",
            "systems/zlink/samples/bingo/server/session/Program.java");
        String sessionAuthenticationSource = sampleJavaSource(
            "Bingo",
            "Server/Session/src/main/java",
            "systems/zlink/samples/bingo/server/session/sessions/handlers/AuthenticateSessionHandler.java");
        String apiHandlerSource = sampleJavaSource(
            "Bingo",
            "Server/Api/src/main/java",
            "systems/zlink/samples/bingo/server/api/handlers/AuthenticatePlayerHandler.java");

        assertTrue(rootBuildSource.contains("plugins {\n    base\n}")
                && !rootBuildSource.contains("application"),
            "Bingo root project must not expose an aggregate in-process runner");
        assertTrue(sharedBuildSource.contains("id(\"com.google.protobuf\")")
                && sharedBuildSource.contains("protobuf-java:4.30.2")
                && sharedBuildSource.contains("protoc:4.30.2"),
            "Java Bingo Shared project must generate protobuf message classes from a checked-in schema");
        assertTrue(sharedProtoSource.contains("option java_outer_classname = \"Messages\";")
                && sharedProtoSource.contains("message AuthenticateReq")
                && sharedProtoSource.contains("message MatchBingoReq")
                && sharedProtoSource.contains("message SubmitBingoCardReq")
                && sharedProtoSource.contains("message BingoGameEndedNotify")
                && sharedProtoSource.contains("message BingoRewardAnnouncedNotify")
                && sharedProtoSource.contains("message BingoRoomState")
                && sharedProtoSource.contains("message BingoPlayerState"),
            "Java Bingo protobuf schema must declare the common Bingo payload messages");
        assertTrue(sharedContractsSource.contains("Messages.MatchBingoReq.newBuilder()")
                && sharedContractsSource.contains("Messages.BingoRoomState.newBuilder()")
                && !sharedContractsSource.contains("record MatchBingoReq")
                && !sharedContractsSource.contains("@ZLinkPacket"),
            "Java Bingo contract helper must expose generated protobuf messages, not hand-written packet DTOs");
        assertTrue(clientProgramSource.contains("ZLinkStreamConnector client1 = createClient(")
                && clientProgramSource.contains("ZLinkStreamConnector client2 = createClient(")
                && clientProgramSource.contains("ZLinkStreamConnector observer = createClient(")
                && clientProgramSource.contains("new BingoClientScenario().run(client1, client2, observer)"),
            "Bingo client Program must create configured player and observer connectors and pass them into the scenario app");
        assertTrue(clientAppSource.contains("public final class BingoClientScenario")
                && !clientAppSource.contains("BingoClientApp"),
            "Bingo client flow must use ClientScenario naming");
        assertTrue(clientProgramSource.contains("client1.close().submit()")
                && clientProgramSource.contains("client2.close().submit()"),
            "Bingo client Program must close connectors through lifecycle call builders");
        assertTrue(clientProgramSource.contains("ZLinkStreamConnectorFactory.create"),
            "Bingo sample must use connector public factory");
        assertTrue(clientProgramSource.contains("ZLinkStreamDispatchMode.IMMEDIATE"),
            "Bingo sample must use configured auto-dispatch connectors like the .NET immediate-dispatch sample");
        assertTrue(clientAppSource.contains("ZLinkStreamConnector client1")
                && clientAppSource.contains("ZLinkStreamConnector client2")
                && clientAppSource.contains("BingoMessages.submitBingoCardReq")
                && clientAppSource.contains("BingoMessages.matchBingoReq(\"two-player\")")
                && clientAppSource.contains("client1.waitFor(SampleNames.PlayerJoinedPacket)")
                && clientAppSource.contains("request(BingoMessages.authenticateReq")
                && clientAppSource.contains(".submit(Messages.AuthenticateRes.class)")
                && clientAppSource.contains(".submit(Messages.PlayerJoinedNotify.class)")
                && clientAppSource.contains("client1SawClient2Join.toCompletableFuture().join()")
                && !clientAppSource.contains("ZLinkProtobufCodec.")
                && clientAppSource.contains("List.of(1, 2, 3, 4, 0, 6, 7, 8, 9)")
                && clientAppSource.contains("client1Result.getWinnersList().equals(List.of(client1Auth.getActorId()))")
                && roomSource.contains("submitCard")
                && roomSource.contains("game.drawNext()"),
            "Bingo sample must use connector member APIs, submit .NET baseline cards, and let the server timer draw numbers");
        assertTrue(clientProgramSource.contains("ZLinkProtobufCodec.defaultCodec()"),
            "Bingo client Program must configure the codec outside the scenario app");
        assertTrue(!clientAppSource.contains("server.play")
                && !roomSource.contains("BingoWinnerSink"),
            "Bingo sample must not couple client notification handling to server implementation types");
        assertTrue(apiHostSource.contains("addHandlersFromPackageOf")
                && apiHostSource.contains("addClientServerChannel(SampleNames.ApiChannel)")
                && apiHostSource.contains("selectedApiChannelEndpoint()")
                && apiHostSource.contains("selectedApiMeshEndpoint()")
                && !apiHostSource.contains("addRequestHandler")
                && playHostSource.contains("addHandlersFromPackageOf")
                && playHostSource.contains("addClientServerChannel(SampleNames.ApiChannel).client()")
                && playHostSource.contains("ZLinkRedisRelocationStore")
                && playHostSource.contains("ZLinkUserSpotExecutionMode.SPOT_WIDE")
                && playHostSource.contains("ZLinkSpotRelocationReadinessMode.APPLICATION_SIGNALED")
                && playHostSource.contains("preserveStateWith(BingoRoomRelocationAdapter.class)")
                && !playHostSource.contains("addRequestHandler"),
            "Bingo Api/Play roles must separate ClientServer from RouteMesh and configure relocatable rooms");
        assertTrue(apiHostSource.contains("setRoutingIdPrefix(\"api\")")
                && playHostSource.contains("setRoutingIdPrefix(\"play\")")
                && sessionHostSource.contains("setRoutingIdPrefix(\"session\")")
                && !apiHostSource.contains("setRoutingId(")
                && !playHostSource.contains("setRoutingId(")
                && !sessionHostSource.contains("setRoutingId(")
                && !apiHostSource.contains("configureEntrySpot")
                && !playHostSource.contains("configureEntrySpot")
                && !sessionHostSource.contains("configureEntrySpot"),
            "Java Bingo roles must allocate routing IDs before bind without entry Spot overrides");
        assertTrue(apiHostSource.contains("@SpringBootApplication")
                && apiHostSource.contains("SampleApplication.start")
                && apiHostSource.contains("ZLinkFrameworkConfigurer")
                && !apiHostSource.contains("ZLinkFramework.start")
                && playHostSource.contains("@SpringBootApplication")
                && playHostSource.contains("SampleApplication.start")
                && playHostSource.contains("ZLinkFrameworkConfigurer")
                && !playHostSource.contains("ZLinkFramework.start")
                && sessionHostSource.contains("@SpringBootApplication")
                && sessionHostSource.contains("SampleApplication.start")
                && sessionHostSource.contains("ZLinkFrameworkConfigurer")
                && !sessionHostSource.contains("ZLinkFramework.start"),
            "Bingo server roles must run ZLink through Spring Boot lifecycle beans");
        assertFalse(apiProgramSource.contains("CountDownLatch")
                || playProgramSource.contains("CountDownLatch")
                || sessionProgramSource.contains("CountDownLatch"),
            "Bingo role entry points must not keep direct ZLink starts alive with CountDownLatch");
        assertTrue(locationStoreSource.contains("ZLinkRedisLocationStore")
                && locationStoreSource.contains("ZLinkRedisLocationOptions")
                && apiHostSource.contains("ZLinkRedisLocationStore locationStore(")
                && playHostSource.contains("ZLinkRedisLocationStore locationStore(")
                && sessionHostSource.contains("ZLinkRedisLocationStore locationStore(")
                && playHostSource.contains("configureLocations()")
                && sessionHostSource.contains("configureLocations()")
                && !apiHostSource.contains("ZLinkEmbeddedRegistryOptions")
                && !playHostSource.contains("ZLinkEmbeddedRegistryOptions")
                && !sessionHostSource.contains("ZLinkEmbeddedRegistryOptions"),
            "Java Bingo roles must use the Redis location store extension instead of a Registry role");
        assertTrue(apiHandlerSource.contains("@ZLinkHandlerGroup(SampleNames.ApiChannel)")
                && apiHandlerSource.contains("implements ZLinkRequestHandler<")
                && apiHandlerSource.contains("Messages.AuthenticatePlayerReq")
                && sessionAuthenticationSource.contains("actors.getOrCreate(")
                && sessionAuthenticationSource.contains("BingoMessages.ensurePlayerActorReq(")
                && sessionAuthenticationSource.contains(".request(")
                && sessionAuthenticationSource.contains(".submit()")
                && entrySpotSource.contains("onCreateActor(")
                && entrySpotSource.contains("createRequest.decode(Messages.EnsurePlayerActorReq.class)")
                && entrySpotSource.contains("actor.setDisplayName("),
            "Bingo authentication must create the Actor through ActorManager and initialize it in the Entry Spot lifecycle");
        assertTrue(sampleFileContains("java", "Bingo", "Server/Api/src/main/java",
                "systems/zlink/samples/bingo/server/api/Program.java", "ApiServerApplication.run"),
            "Java Bingo Api role must have its own executable Program");
        assertTrue(sampleFileContains("java", "Bingo", "Server/Play/src/main/java",
                "systems/zlink/samples/bingo/server/play/Program.java", "PlayServerApplication.run"),
            "Java Bingo Play role must have its own executable Program");
        assertTrue(sampleFileContains("java", "Bingo", "Server/Session/src/main/java",
                "systems/zlink/samples/bingo/server/session/Program.java", "SessionServerApplication.run"),
            "Java Bingo Session role must have its own executable Program");
        assertFalse(roomSource.contains("BingoPlayerClient"),
            "Bingo server push must go through framework bound sessions, not direct client objects");
    }

    @Test
    void bingoKotlinSampleMirrorsJavaRoleLayout() throws IOException {
        SampleSourcePaths paths = kotlinSamplePaths("bingo");
        assertNoSampleSourcesUnder("kotlin", "Bingo", "src/main/kotlin");
        assertSampleFilesExist("kotlin", "Bingo", "Client/src/main/kotlin", List.of(
            "systems/zlink/samples/kotlin/bingo/client/Program.kt",
            "systems/zlink/samples/kotlin/bingo/client/BingoClientScenario.kt"));
        assertSampleFilesExist("kotlin", "Bingo", "Server/Api/src/main/kotlin", List.of(
            "systems/zlink/samples/kotlin/bingo/server/api/Program.kt",
            "systems/zlink/samples/kotlin/bingo/server/api/ApiServerApplication.kt",
            "systems/zlink/samples/kotlin/bingo/server/api/handlers/AuthenticatePlayerHandler.kt",
            "systems/zlink/samples/kotlin/bingo/server/api/handlers/MatchBingoHandler.kt"));
        assertSampleFilesExist("kotlin", "Bingo", "Server/Play/src/main/kotlin", List.of(
            "systems/zlink/samples/kotlin/bingo/server/play/Program.kt",
            "systems/zlink/samples/kotlin/bingo/server/play/PlayServerApplication.kt",
            "systems/zlink/samples/kotlin/bingo/server/play/infrastructure/zlink/actors/PlayerActor.kt",
            "systems/zlink/samples/kotlin/bingo/server/play/infrastructure/zlink/actors/PlayerActorFactory.kt",
            "systems/zlink/samples/kotlin/bingo/server/play/domain/bingo/BingoCard.kt",
            "systems/zlink/samples/kotlin/bingo/server/play/domain/bingo/BingoRoomModels.kt",
            paths.playSpot("bingoroomspot", "BingoRoomSpot"),
            paths.playSpotHandler("bingoroomspot", "BingoRoomSettingsInitializer"),
            paths.playSpotHandler("bingoroomspot", "BingoRoomTimerHandler"),
            paths.playSpotHandler("bingoroomspot", "SubmitBingoCardHandler"),
            paths.playSpot("bingoroomspot", "BingoRoomRelocationAdapter"),
            paths.playSpot("entryspot", "BingoEntrySpot"),
            paths.playSpotHandler("entryspot", "MatchBingoActorHandler")));
        assertSampleFilesExist("kotlin", "Bingo", "Server/Matchmaking/src/main/kotlin", List.of(
            "systems/zlink/samples/kotlin/bingo/server/matchmaking/MatchmakingServerApplication.kt",
            "systems/zlink/samples/kotlin/bingo/server/matchmaking/BingoMatchmaker.kt",
            "systems/zlink/samples/kotlin/bingo/server/matchmaking/BingoMatchmakerIdleTimerHandler.kt",
            "systems/zlink/samples/kotlin/bingo/server/matchmaking/ReserveBingoRoomHandler.kt"));
        assertSampleFilesExist("kotlin", "Bingo", "Server/Session/src/main/kotlin", List.of(
            "systems/zlink/samples/kotlin/bingo/server/session/Program.kt",
            "systems/zlink/samples/kotlin/bingo/server/session/SessionServerApplication.kt",
            "systems/zlink/samples/kotlin/bingo/server/session/sessions/BingoSession.kt",
            "systems/zlink/samples/kotlin/bingo/server/session/sessions/handlers/AuthenticateSessionHandler.kt"));
        assertSampleFilesExist("kotlin", "Bingo", "Server/Configuration/src/main/kotlin", List.of(
            "systems/zlink/samples/kotlin/bingo/server/configuration/SampleLocationStore.kt",
            "systems/zlink/samples/kotlin/bingo/server/configuration/SampleNames.kt",
            "systems/zlink/samples/kotlin/bingo/server/configuration/SampleTopology.kt",
            "systems/zlink/samples/kotlin/bingo/server/configuration/SampleTimings.kt"));
        assertSampleFilesExist("kotlin", "Bingo", "Client/src/main/kotlin", List.of(
            "systems/zlink/samples/kotlin/bingo/client/configuration/SampleNames.kt",
            "systems/zlink/samples/kotlin/bingo/client/configuration/SampleTopology.kt",
            "systems/zlink/samples/kotlin/bingo/client/configuration/SampleTimings.kt"));
        assertSampleFilesExist("kotlin", "Bingo", "Shared/src/main/kotlin", List.of(
            "systems/zlink/samples/kotlin/bingo/shared/contracts/Messages.kt"));
        assertSampleFilesExist("kotlin", "Bingo", "Shared/src/main/proto", List.of(
            "bingo_messages.proto"));

        String rootBuildSource = sampleFile(
            "kotlin",
            "Bingo",
            "",
            "build.gradle.kts");
        String sharedBuildSource = sampleFile(
            "kotlin",
            "Bingo",
            "Shared",
            "build.gradle.kts");
        String sharedContractsSource = sampleKotlinSource(
            "Bingo",
            "Shared/src/main/kotlin",
            "systems/zlink/samples/kotlin/bingo/shared/contracts/Messages.kt");
        String sharedProtoSource = sampleFile(
            "kotlin",
            "Bingo",
            "Shared/src/main/proto",
            "bingo_messages.proto");
        String clientProgramSource = sampleKotlinSource(
            "Bingo",
            "Client/src/main/kotlin",
            "systems/zlink/samples/kotlin/bingo/client/Program.kt");
        String clientAppSource = sampleKotlinSource(
            "Bingo",
            "Client/src/main/kotlin",
            "systems/zlink/samples/kotlin/bingo/client/BingoClientScenario.kt");
        String roomSource = sampleKotlinSource(
            "Bingo",
            "Server/Play/src/main/kotlin",
            paths.playSpot("bingoroomspot", "BingoRoomSpot"));
        String apiHostSource = sampleKotlinSource(
            "Bingo",
            "Server/Api/src/main/kotlin",
            "systems/zlink/samples/kotlin/bingo/server/api/ApiServerApplication.kt");
        String playHostSource = sampleKotlinSource(
            "Bingo",
            "Server/Play/src/main/kotlin",
            "systems/zlink/samples/kotlin/bingo/server/play/PlayServerApplication.kt");
        String sessionHostSource = sampleKotlinSource(
            "Bingo",
            "Server/Session/src/main/kotlin",
            "systems/zlink/samples/kotlin/bingo/server/session/SessionServerApplication.kt");
        String locationStoreSource = sampleKotlinSource(
            "Bingo",
            "Server/Configuration/src/main/kotlin",
            "systems/zlink/samples/kotlin/bingo/server/configuration/SampleLocationStore.kt");
        String apiProgramSource = sampleKotlinSource(
            "Bingo",
            "Server/Api/src/main/kotlin",
            "systems/zlink/samples/kotlin/bingo/server/api/Program.kt");
        String playProgramSource = sampleKotlinSource(
            "Bingo",
            "Server/Play/src/main/kotlin",
            "systems/zlink/samples/kotlin/bingo/server/play/Program.kt");
        String entrySpotSource = sampleKotlinSource(
            "Bingo",
            "Server/Play/src/main/kotlin",
            paths.playSpot("entryspot", "BingoEntrySpot"));
        String sessionProgramSource = sampleKotlinSource(
            "Bingo",
            "Server/Session/src/main/kotlin",
            "systems/zlink/samples/kotlin/bingo/server/session/Program.kt");
        String sessionAuthenticationSource = sampleKotlinSource(
            "Bingo",
            "Server/Session/src/main/kotlin",
            "systems/zlink/samples/kotlin/bingo/server/session/sessions/handlers/AuthenticateSessionHandler.kt");
        String apiHandlerSource = sampleKotlinSource(
            "Bingo",
            "Server/Api/src/main/kotlin",
            "systems/zlink/samples/kotlin/bingo/server/api/handlers/AuthenticatePlayerHandler.kt");

        assertTrue(rootBuildSource.contains("plugins {\n    base\n}")
                && !rootBuildSource.contains("application"),
            "Kotlin Bingo root project must not expose an aggregate in-process runner");
        assertTrue(sharedBuildSource.contains("id(\"com.google.protobuf\")")
                && sharedBuildSource.contains("protobuf-java:4.30.2")
                && sharedBuildSource.contains("protoc:4.30.2"),
            "Kotlin Bingo Shared project must generate protobuf message classes from a checked-in schema");
        assertTrue(sharedProtoSource.contains("option java_outer_classname = \"Messages\";")
                && sharedProtoSource.contains("message AuthenticateReq")
                && sharedProtoSource.contains("message MatchBingoReq")
                && sharedProtoSource.contains("message SubmitBingoCardReq")
                && sharedProtoSource.contains("message BingoGameEndedNotify")
                && sharedProtoSource.contains("message BingoRewardAnnouncedNotify")
                && sharedProtoSource.contains("message BingoRoomState")
                && sharedProtoSource.contains("message BingoPlayerState"),
            "Kotlin Bingo protobuf schema must declare the common Bingo payload messages");
        assertTrue(sharedContractsSource.contains("typealias MatchBingoReq = Messages.MatchBingoReq")
                && sharedContractsSource.contains("fun MatchBingoReq(mode: String): MatchBingoReq")
                && sharedContractsSource.contains("typealias BingoRoomState = Messages.BingoRoomState")
                && !sharedContractsSource.contains("data class MatchBingoReq")
                && !sharedContractsSource.contains("@ZLinkPacket"),
            "Kotlin Bingo contract source must expose generated protobuf messages, not hand-written packet DTOs");
        assertTrue(clientProgramSource.contains("val client1 = createClient(")
                && clientProgramSource.contains("val client2 = createClient(")
                && clientProgramSource.contains("val observer = createClient(")
                && clientProgramSource.contains("BingoClientScenario().run(client1, client2, observer)"),
            "Kotlin Bingo client Program must create configured player and observer connectors and pass them into the scenario app");
        assertTrue(clientAppSource.contains("class BingoClientScenario")
                && !clientAppSource.contains("BingoClientApp"),
            "Kotlin Bingo client flow must use ClientScenario naming");
        assertTrue(clientProgramSource.contains(".kotlin()")
                && clientProgramSource.contains("client1.close().await()")
                && clientProgramSource.contains("client2.close().await()"),
            "Kotlin Bingo client Program must close connectors through coroutine lifecycle wrappers");
        assertTrue(clientProgramSource.contains("ZLinkStreamConnectorFactory.create"),
            "Kotlin Bingo sample must use connector public factory");
        assertTrue(clientProgramSource.contains("ZLinkStreamDispatchMode.IMMEDIATE"),
            "Kotlin Bingo sample must use configured auto-dispatch connectors like the .NET immediate-dispatch sample");
        assertTrue(clientAppSource.contains("client1: ZLinkKotlinStreamConnector")
                && clientAppSource.contains("client2: ZLinkKotlinStreamConnector")
                && clientAppSource.contains("SubmitBingoCardReq")
                && clientAppSource.contains("MatchBingoReq(\"two-player\")")
                && clientAppSource.contains("client1.waitFor<PlayerJoinedNotify>()")
                && clientAppSource.contains("val client1Draws = (1..15).map { expectedDrawSeq ->")
                && clientAppSource.contains(".where { message -> message.payload().drawSeq == expectedDrawSeq }")
                && clientAppSource.contains("client1Draws.drop(drawnNumbers.size).forEach { wait -> wait.cancel() }")
                && clientAppSource.contains(".request(AuthenticateReq")
                && clientAppSource.contains(".awaitReply<AuthenticateRes>()")
                && !clientAppSource.contains(".submit()")
                && !clientAppSource.contains("ZLinkProtobufCodec.")
                && clientAppSource.contains("listOf(1, 2, 3, 4, 0, 6, 7, 8, 9)")
                && clientAppSource.contains("client1Result.winners == listOf(client1Auth.actorId)")
                && roomSource.contains("submitCard")
                && roomSource.contains("game.drawNext()"),
            "Kotlin Bingo sample must use connector member APIs, submit .NET baseline cards, and let the server timer draw numbers");
        assertTrue(clientProgramSource.contains("ZLinkProtobufCodec.defaultCodec()"),
            "Kotlin Bingo client Program must configure the codec outside the scenario app");
        assertTrue(!clientAppSource.contains("server.play")
                && !roomSource.contains("BingoWinnerSink"),
            "Kotlin Bingo sample must not couple client notification handling to server implementation types");
        assertTrue(apiHostSource.contains("addHandlersFromPackageOf")
                && apiHostSource.contains("addClientServerChannel(SampleNames.ApiChannel)")
                && apiHostSource.contains("selectedApiChannelEndpoint()")
                && apiHostSource.contains("selectedApiMeshEndpoint()")
                && !apiHostSource.contains("addRequestHandler")
                && playHostSource.contains("addHandlersFromPackageOf")
                && playHostSource.contains("addClientServerChannel(SampleNames.ApiChannel).client()")
                && playHostSource.contains("ZLinkRedisRelocationStore")
                && playHostSource.contains("ZLinkUserSpotExecutionMode.SPOT_WIDE")
                && playHostSource.contains("ZLinkSpotRelocationReadinessMode.APPLICATION_SIGNALED")
                && playHostSource.contains("preserveStateWith(BingoRoomRelocationAdapter::class.java)")
                && !playHostSource.contains("addRequestHandler"),
            "Kotlin Bingo Api/Play roles must separate ClientServer from RouteMesh and configure relocatable rooms");
        assertTrue(apiHostSource.contains("setRoutingIdPrefix(\"api\")")
                && playHostSource.contains("setRoutingIdPrefix(\"play\")")
                && sessionHostSource.contains("setRoutingIdPrefix(\"session\")")
                && !apiHostSource.contains("setRoutingId(")
                && !playHostSource.contains("setRoutingId(")
                && !sessionHostSource.contains("setRoutingId(")
                && !apiHostSource.contains("configureEntrySpot")
                && !playHostSource.contains("configureEntrySpot")
                && !sessionHostSource.contains("configureEntrySpot"),
            "Kotlin Bingo roles must allocate routing IDs before bind without entry Spot overrides");
        assertTrue(apiHostSource.contains("@SpringBootApplication")
                && apiHostSource.contains("SpringApplicationBuilder")
                && apiHostSource.contains("ZLinkFrameworkConfigurer")
                && !apiHostSource.contains("ZLinkFramework.start")
                && playHostSource.contains("@SpringBootApplication")
                && playHostSource.contains("SpringApplicationBuilder")
                && playHostSource.contains("ZLinkFrameworkConfigurer")
                && !playHostSource.contains("ZLinkFramework.start")
                && sessionHostSource.contains("@SpringBootApplication")
                && sessionHostSource.contains("SpringApplicationBuilder")
                && sessionHostSource.contains("ZLinkFrameworkConfigurer")
                && !sessionHostSource.contains("ZLinkFramework.start"),
            "Kotlin Bingo server roles must run ZLink through Spring Boot lifecycle beans");
        assertFalse(apiProgramSource.contains("CountDownLatch")
                || playProgramSource.contains("CountDownLatch")
                || sessionProgramSource.contains("CountDownLatch"),
            "Kotlin Bingo role entry points must not keep direct ZLink starts alive with CountDownLatch");
        assertTrue(locationStoreSource.contains("ZLinkRedisLocationStore")
                && locationStoreSource.contains("ZLinkRedisLocationOptions")
                && apiHostSource.contains("fun locationStore(")
                && apiHostSource.contains("): ZLinkRedisLocationStore")
                && playHostSource.contains("fun locationStore(")
                && playHostSource.contains("): ZLinkRedisLocationStore")
                && sessionHostSource.contains("fun locationStore(")
                && sessionHostSource.contains("): ZLinkRedisLocationStore")
                && !apiHostSource.contains("ZLinkEmbeddedRegistryOptions")
                && !playHostSource.contains("ZLinkEmbeddedRegistryOptions")
                && !sessionHostSource.contains("ZLinkEmbeddedRegistryOptions"),
            "Kotlin Bingo roles must use the Redis location store extension instead of a Registry role");
        assertTrue(apiHandlerSource.contains("@ZLinkHandlerGroup(SampleNames.ApiChannel)")
                && apiHandlerSource.contains(": ZLinkSuspendingRequestHandler<AuthenticatePlayerReq, AuthenticatePlayerRes>")
                && sessionAuthenticationSource.contains("actors.kotlin().getOrCreate(")
                && sessionAuthenticationSource.contains("EnsurePlayerActorReq(")
                && sessionAuthenticationSource.contains(".request(")
                && sessionAuthenticationSource.contains(".await()")
                && entrySpotSource.contains("onCreateActorSuspending(")
                && entrySpotSource.contains("createRequest.decode(EnsurePlayerActorReq::class.java)")
                && entrySpotSource.contains("actor.setDisplayName("),
            "Kotlin Bingo authentication must create the Actor through ActorManager and initialize it in the Entry Spot lifecycle");
        assertTrue(sampleFileContains("kotlin", "Bingo", "Server/Api/src/main/kotlin",
                "systems/zlink/samples/kotlin/bingo/server/api/Program.kt", "ApiServerApplication.run"),
            "Kotlin Bingo Api role must have its own executable Program");
        assertTrue(sampleFileContains("kotlin", "Bingo", "Server/Play/src/main/kotlin",
                "systems/zlink/samples/kotlin/bingo/server/play/Program.kt", "PlayServerApplication.run"),
            "Kotlin Bingo Play role must have its own executable Program");
        assertTrue(sampleFileContains("kotlin", "Bingo", "Server/Session/src/main/kotlin",
                "systems/zlink/samples/kotlin/bingo/server/session/Program.kt", "SessionServerApplication.run"),
            "Kotlin Bingo Session role must have its own executable Program");
        assertFalse(roomSource.contains("BingoPlayerClient"),
            "Kotlin Bingo server push must go through framework bound sessions, not direct client objects");
    }

    @Test
    void maintainedSamplesImplementActorLifecycleScenarios() throws IOException {
        SampleSourcePaths javaBingo = javaSamplePaths("bingo");
        SampleSourcePaths javaTicTacToe = javaSamplePaths("tictactoe");
        SampleSourcePaths kotlinBingo = kotlinSamplePaths("bingo");
        SampleSourcePaths kotlinTicTacToe = kotlinSamplePaths("tictactoe");
        assertJavaActorLifecycleSpec(
            "Bingo",
            "Server/Play/src/main/java",
            javaBingo.playSpot("entryspot", "BingoEntrySpot"),
            javaBingo.playSpot("bingoroomspot", "BingoRoomSpot"),
            "systems/zlink/samples/bingo/server/play/infrastructure/zlink/actors/PlayerActor.java",
            "Server/Session/src/main/java",
            "systems/zlink/samples/bingo/server/session/sessions/BingoSession.java");
        assertJavaActorLifecycleSpec(
            "TicTacToe",
            "Server/src/main/java",
            javaTicTacToe.playSpot("entryspot", "PlayEntrySpot"),
            javaTicTacToe.playSpot("tictactoegamespot", "TicTacToeGame"),
            "systems/zlink/samples/tictactoe/server/play/infrastructure/zlink/actors/PlayActor.java",
            "Server/src/main/java",
            "systems/zlink/samples/tictactoe/server/play/infrastructure/zlink/sessions/PlaySession.java");
        assertKotlinActorLifecycleSpec(
            "Bingo",
            "Server/Play/src/main/kotlin",
            kotlinBingo.playSpot("entryspot", "BingoEntrySpot"),
            kotlinBingo.playSpot("bingoroomspot", "BingoRoomSpot"),
            "systems/zlink/samples/kotlin/bingo/server/play/infrastructure/zlink/actors/PlayerActor.kt",
            "Server/Session/src/main/kotlin",
            "systems/zlink/samples/kotlin/bingo/server/session/sessions/BingoSession.kt");
        assertKotlinActorLifecycleSpec(
            "TicTacToe",
            "Server/src/main/kotlin",
            kotlinTicTacToe.playSpot("entryspot", "PlayEntrySpot"),
            kotlinTicTacToe.playSpot("tictactoegamespot", "TicTacToeGame"),
            "systems/zlink/samples/kotlin/tictactoe/server/play/infrastructure/zlink/actors/PlayActor.kt",
            "Server/src/main/kotlin",
            "systems/zlink/samples/kotlin/tictactoe/server/play/infrastructure/zlink/sessions/PlaySession.kt");
    }

    private static void assertJavaActorLifecycleSpec(
        String sample,
        String playSourceRoot,
        String entrySpotPath,
        String userSpotPath,
        String actorPath,
        String sessionSourceRoot,
        String sessionPath) throws IOException {
        String entrySpotSource = sampleJavaSource(sample, playSourceRoot, entrySpotPath);
        String userSpotSource = sampleJavaSource(sample, playSourceRoot, userSpotPath);
        String actorSource = sampleJavaSource(sample, playSourceRoot, actorPath);
        String sessionSource = sampleJavaSource(sample, sessionSourceRoot, sessionPath);

        assertActorLifecycleSpec(
            sample,
            entrySpotSource,
            userSpotSource,
            actorSource,
            sessionSource,
            List.of("CompletionStage<Void> onDisconnected()", "notifyDisconnected()"));
    }

    private static void assertKotlinActorLifecycleSpec(
        String sample,
        String playSourceRoot,
        String entrySpotPath,
        String userSpotPath,
        String actorPath,
        String sessionSourceRoot,
        String sessionPath) throws IOException {
        String entrySpotSource = sampleKotlinSource(sample, playSourceRoot, entrySpotPath);
        String userSpotSource = sampleKotlinSource(sample, playSourceRoot, userSpotPath);
        String actorSource = sampleKotlinSource(sample, playSourceRoot, actorPath);
        String sessionSource = sampleKotlinSource(sample, sessionSourceRoot, sessionPath);

        assertActorLifecycleSpec(
            sample,
            entrySpotSource,
            userSpotSource,
            actorSource,
            sessionSource,
            List.of("onDisconnectedSuspending", "notifyDisconnected().await()"));
    }

    private static void assertActorLifecycleSpec(
        String sample,
        String entrySpotSource,
        String userSpotSource,
        String actorSource,
        String sessionSource,
        List<String> disconnectCleanupNeedles) {
        for (String needle : List.of(
            "onCreateActor",
            "onJoinedActor",
            "onDisconnectActor",
            "destroyActor(",
            "destroyAfterEntrySpotJoin",
            "markDisconnected")) {
            assertTrue(entrySpotSource.contains(needle), sample + " Entry Spot must include " + needle);
        }
        for (String needle : List.of(
            "onLeaveActor",
            "onDisconnectActor",
            "leaveActor(",
            "markForDestroyAfterRoomLeave",
            "markDisconnected")) {
            assertTrue(userSpotSource.contains(needle), sample + " user Spot must include " + needle);
        }
        assertFalse(userSpotSource.contains("destroyActor("),
            sample + " user Spot must not destroy actors directly");
        for (String needle : List.of(
            "destroyAfterEntrySpotJoin",
            "markForDestroyAfterRoomLeave",
            "markDisconnected",
            "disconnected")) {
            assertTrue(actorSource.contains(needle), sample + " actor must include " + needle);
        }
        for (String needle : disconnectCleanupNeedles) {
            assertTrue(sessionSource.contains(needle), sample + " session disconnect must include " + needle);
        }
        assertFalse(sessionSource.contains("leaveActor(") || sessionSource.contains("destroyActor("),
            sample + " session disconnect must not leave rooms or destroy actors");
    }

    private static Path samplesRoot() {
        return frameworkJavaRoot().resolve("samples");
    }

    private static Path frameworkJavaRoot() {
        return Path.of(System.getProperty("user.dir")).getParent();
    }

    private static SampleSourcePaths javaSamplePaths(String packageName) {
        return new SampleSourcePaths("systems/zlink/samples/" + packageName, ".java");
    }

    private static SampleSourcePaths kotlinSamplePaths(String packageName) {
        return new SampleSourcePaths("systems/zlink/samples/kotlin/" + packageName, ".kt");
    }

    private record SampleSourcePaths(String packageRoot, String extension) {
        String playSpot(String spotDirectory, String className) {
            return playZLink("spots/" + spotDirectory + "/" + className);
        }

        String playSpotHandler(String spotDirectory, String className) {
            return playZLink("spots/" + spotDirectory + "/handlers/" + className);
        }

        private String playZLink(String relativePath) {
            return packageRoot + "/server/play/infrastructure/zlink/" + relativePath + extension;
        }
    }

    private static List<Path> officialActorDestroyDocs() throws IOException {
        List<Path> docs = new java.util.ArrayList<>();
        for (String relativeRoot : List.of("../../doc/framework/java/guide", "../../doc/framework/java/spec", "../../doc/framework/java/internals")) {
            Path root = frameworkJavaRoot().resolve(relativeRoot);
            if (!Files.exists(root)) {
                continue;
            }
            try (Stream<Path> files = Files.walk(root)) {
                files
                    .filter(Files::isRegularFile)
                    .filter(path -> path.getFileName().toString().endsWith(".md"))
                    .forEach(docs::add);
            }
        }
        try (Stream<Path> files = Files.walk(samplesRoot())) {
            files
                .filter(Files::isRegularFile)
                .filter(path -> path.getFileName().toString().equals("README.md")
                    || path.getFileName().toString().equals("README.ko.md"))
                .forEach(docs::add);
        }
        return docs;
    }

    private static String sampleJavaSource(String sample, String relativePath) throws IOException {
        return sampleJavaSource(sample, "src/main/java", relativePath);
    }

    private static String sampleJavaSource(
        String sample,
        String sourceRoot,
        String relativePath) throws IOException {
        return Files.readString(samplesRoot()
            .resolve("java")
            .resolve(sample)
            .resolve(sourceRoot)
            .resolve(relativePath));
    }

    private static String sampleKotlinSource(String sample, String relativePath) throws IOException {
        return sampleKotlinSource(sample, "src/main/kotlin", relativePath);
    }

    private static String sampleKotlinSource(
        String sample,
        String sourceRoot,
        String relativePath) throws IOException {
        return Files.readString(samplesRoot()
            .resolve("kotlin")
            .resolve(sample)
            .resolve(sourceRoot)
            .resolve(relativePath));
    }

    private static boolean sampleFileContains(
        String language,
        String sample,
        String sourceRoot,
        String relativePath,
        String needle) throws IOException {
        Path file = samplesRoot()
            .resolve(language)
            .resolve(sample)
            .resolve(sourceRoot)
            .resolve(relativePath);
        return Files.isRegularFile(file) && Files.readString(file).contains(needle);
    }

    private static String sampleFile(
        String language,
        String sample,
        String sourceRoot,
        String relativePath) throws IOException {
        return Files.readString(samplesRoot()
            .resolve(language)
            .resolve(sample)
            .resolve(sourceRoot)
            .resolve(relativePath));
    }

    private static void assertDotNetProjectLayout(Path sampleRoot, List<String> relativeProjects) {
        for (String relativeProject : relativeProjects) {
            Path projectRoot = sampleRoot.resolve(relativeProject);
            assertTrue(Files.isDirectory(projectRoot),
                "missing .NET-parity project folder " + projectRoot);
            assertTrue(Files.isRegularFile(projectRoot.resolve("build.gradle.kts")),
                "missing Gradle project for .NET-parity folder " + projectRoot);
        }
    }

    private static void assertSharedProjectContainsOnlyContracts(Path sampleRoot) throws IOException {
        Path sharedSourceRoot = sampleRoot.resolve("Shared/src/main");
        try (Stream<Path> files = Files.walk(sharedSourceRoot)) {
            List<Path> nonContractSources = files
                .filter(Files::isRegularFile)
                .filter(SampleReleaseGateContractTest::isSampleSource)
                .filter(path -> !path.toString().replace('\\', '/').contains("/contracts/"))
                .toList();

            assertTrue(nonContractSources.isEmpty(),
                sampleRoot.getFileName() + " Shared project must contain only message contracts: "
                    + nonContractSources);
        }
    }

    private static void assertSampleFilesExist(
        String language,
        String sample,
        String sourceRoot,
        List<String> relativePaths) {
        Path sampleSourceRoot = samplesRoot()
            .resolve(language)
            .resolve(sample)
            .resolve(sourceRoot);
        for (String relativePath : relativePaths) {
            assertTrue(Files.isRegularFile(sampleSourceRoot.resolve(relativePath)),
                "missing " + language + "/" + sample + " sample source " + relativePath);
        }
    }

    private static void assertNoSampleSourcesUnder(
        String language,
        String sample,
        String sourceRoot,
        List<String> relativeDirectories) throws IOException {
        Path sampleSourceRoot = samplesRoot()
            .resolve(language)
            .resolve(sample)
            .resolve(sourceRoot);
        for (String relativeDirectory : relativeDirectories) {
            Path directory = sampleSourceRoot.resolve(relativeDirectory);
            if (!Files.exists(directory)) {
                continue;
            }
            try (var paths = Files.walk(directory)) {
                assertTrue(paths.noneMatch(SampleReleaseGateContractTest::isSampleSource),
                    "role source must not remain under aggregate source root: "
                        + language + "/" + sample + "/" + sourceRoot + "/" + relativeDirectory);
            }
        }
    }

    private static void assertNoSampleSourcesUnder(
        String language,
        String sample,
        String sourceRoot) throws IOException {
        Path sampleSourceRoot = samplesRoot()
            .resolve(language)
            .resolve(sample)
            .resolve(sourceRoot);
        if (!Files.exists(sampleSourceRoot)) {
            return;
        }
        try (var paths = Files.walk(sampleSourceRoot)) {
            assertTrue(paths.noneMatch(SampleReleaseGateContractTest::isSampleSource),
                "aggregate source root must not contain sample sources: "
                    + language + "/" + sample + "/" + sourceRoot);
        }
    }

    private static boolean isSampleSource(Path path) {
        String pathText = path.toString().replace('\\', '/');
        if (pathText.contains("/build/") || pathText.contains("/bin/") || pathText.contains("/.gradle/")) {
            return false;
        }
        return pathText.endsWith(".java") || pathText.endsWith(".kt");
    }

    private static void assertSourceContains(Path root, String extension, String expected) throws IOException {
        try (Stream<Path> files = Files.walk(root)) {
            boolean found = files
                .filter(Files::isRegularFile)
                .filter(path -> path.toString().endsWith(extension))
                .filter(SampleReleaseGateContractTest::isSampleSource)
                .anyMatch(path -> sourceContains(path, expected));
            assertTrue(found, root + " must expose common sample message " + expected);
        }
    }

    private static void assertSourceDoesNotContain(Path root, String extension, String forbidden) throws IOException {
        try (Stream<Path> files = Files.walk(root)) {
            List<Path> offenders = files
                .filter(Files::isRegularFile)
                .filter(path -> path.toString().endsWith(extension))
                .filter(SampleReleaseGateContractTest::isSampleSource)
                .filter(path -> sourceContains(path, forbidden))
                .toList();
            assertTrue(offenders.isEmpty(), root + " must not expose obsolete sample message "
                + forbidden + ": " + offenders);
        }
    }

    private static boolean sourceContains(Path path, String text) {
        try {
            return Files.readString(path).contains(text);
        } catch (IOException ex) {
            throw new IllegalStateException("failed to read " + path, ex);
        }
    }

    private static boolean isServerRoleSource(Path path) {
        String normalized = path.toString().replace('\\', '/');
        return normalized.contains("/Server/Api/")
            || normalized.contains("/Server/Play/")
            || normalized.contains("/Server/Registry/")
            || normalized.contains("/Server/Session/");
    }

    private static List<String> forbiddenDirectServerStarts(Path path) {
        try {
            String content = Files.readString(path);
            return List.of(
                    "ZLinkFramework.start",
                    "ZLinkRegistry.start",
                    "CountDownLatch")
                .stream()
                .filter(content::contains)
                .toList();
        } catch (IOException ex) {
            throw new IllegalStateException("failed to read " + path, ex);
        }
    }

    private static boolean isNotSpringBootHostFactory(Path path) {
        try {
            String content = Files.readString(path);
            return !content.contains("@SpringBootApplication")
                || !content.contains("SpringApplicationBuilder");
        } catch (IOException ex) {
            throw new IllegalStateException("failed to read " + path, ex);
        }
    }

    private static List<String> forbiddenLines(Path path) {
        try {
            String content = Files.readString(path);
            return FORBIDDEN_SAMPLE_PATTERNS.stream()
                .filter(content::contains)
                .toList();
        } catch (IOException ex) {
            throw new IllegalStateException("failed to read " + path, ex);
        }
    }

    private static List<String> forbiddenKotlinHandlerRegistrations(Path path) {
        try {
            String content = Files.readString(path);
            return FORBIDDEN_KOTLIN_HANDLER_REGISTRATION
                .matcher(content)
                .results()
                .map(match -> match.group().replaceAll("\\s+", " "))
                .toList();
        } catch (IOException ex) {
            throw new IllegalStateException("failed to read " + path, ex);
        }
    }

    private static List<String> javaManualSpotHandlerRegistrations(Path path) {
        try {
            String content = Files.readString(path);
            return JAVA_MANUAL_SPOT_HANDLER_REGISTRATION
                .matcher(content)
                .results()
                .map(match -> match.group().replaceAll("\\s+", " "))
                .toList();
        } catch (IOException ex) {
            throw new IllegalStateException("failed to read " + path, ex);
        }
    }

    private static List<String> kotlinManualSpotHandlerRegistrations(Path path) {
        try {
            String content = Files.readString(path);
            return KOTLIN_MANUAL_SPOT_HANDLER_REGISTRATION
                .matcher(content)
                .results()
                .map(match -> match.group().replaceAll("\\s+", " "))
                .toList();
        } catch (IOException ex) {
            throw new IllegalStateException("failed to read " + path, ex);
        }
    }
}
