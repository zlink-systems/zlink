package systems.zlink.framework.kotlin

import java.nio.file.Files
import java.nio.file.Path
import kotlin.io.path.readText
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertTrue
import org.junit.jupiter.api.Test

class KotlinJavaTerminalDetektContractTest {
    @Test
    fun forbiddenMethodConfigurationMatchesTheKotlinContractSurfaces() {
        val javaRoot =
            generateSequence(Path.of("").toAbsolutePath()) { it.parent }
                .first {
                    Files.isRegularFile(it.resolve("config/detekt/kotlin-java-terminals.yml"))
                }
        val config = javaRoot.resolve("config/detekt/kotlin-java-terminals.yml").readText()
        val kotlinSpec =
            javaRoot.resolve("../../doc/framework/common/spec/server/languages/kotlin/interfaces")

        val requiredSpecText =
            mapOf(
                "configuration-host.ko.md" to
                    listOf(
                        "application type을 `Class` 인자로",
                        "addHandlersFromPackageOf",
                        "addSessionPacketHandler",
                    ),
                "channel-messaging.ko.md" to
                    listOf(
                        "Kotlin application은 Java Channel call을 직접 사용하지 않는다.",
                        "`.submit().await()`를 작성하지 않는다.",
                    ),
                "spots.ko.md" to listOf("`CompletionStage`와 `Class<T>`를 application에 노출하지 않는다."),
                "actors.ko.md" to listOf("`await(): Unit`만 제공하고 `yield()`를 제공하지 않는다."),
                "stream-session.ko.md" to listOf("Java `CompletionStage`와 submission result type을"),
            )
        requiredSpecText.forEach { (file, fragments) ->
            val text = kotlinSpec.resolve(file).readText()
            fragments.forEach { fragment ->
                assertTrue(text.contains(fragment), "$file no longer declares: $fragment")
            }
        }

        val configuredMethods = Regex("'([^']+)'").findAll(config).map { it.groupValues[1] }.toSet()
        val expectedMethods =
            """
            systems.zlink.framework.configuration.ZLinkFrameworkOptions.addHandlersFromPackageOf(java.lang.Class)
            systems.zlink.framework.configuration.ZLinkFrameworkOptions.useFilter(java.lang.Class)
            systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder.addEntrySpot(java.lang.Class)
            systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder.addSpotFactory(java.lang.String,java.lang.Class,java.util.function.Consumer)
            systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder.addInstanceSpotFactory(java.lang.String,java.lang.Class,java.util.function.Consumer)
            systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder.addActorFactory(java.lang.String,java.lang.Class,java.lang.Class,java.util.function.Consumer)
            systems.zlink.framework.configuration.ZLinkActorFactoryBuilder.preserveStateWith(java.lang.Class)
            systems.zlink.framework.configuration.ZLinkUserSpotFactoryBuilder.preserveStateWith(java.lang.Class)
            systems.zlink.framework.configuration.ZLinkInstanceSpotFactoryBuilder.preserveStateWith(java.lang.Class)
            systems.zlink.framework.configuration.FanoutChannelBuilder.addPublishHandler(java.lang.Class,java.lang.Class)
            systems.zlink.framework.configuration.FanoutChannelBuilder.addPublishHandler(java.lang.Class,java.lang.Class,java.lang.String)
            systems.zlink.framework.configuration.FanoutChannelBuilder.addPublishHandler(java.lang.Class)
            systems.zlink.framework.configuration.FanoutChannelBuilder.addPublishHandler(java.lang.Class,java.lang.String)
            systems.zlink.framework.configuration.ZLinkStreamNodeBuilder.registerSession(java.lang.Class)
            systems.zlink.framework.configuration.ZLinkStreamNodeBuilder.addSessionPacketHandler(java.lang.Class)
            systems.zlink.framework.configuration.ZLinkMeshNodeBuilder.addRouteSendHandler(java.lang.Class,java.lang.Class)
            systems.zlink.framework.configuration.ZLinkMeshNodeBuilder.addRouteRequestHandler(java.lang.Class,java.lang.Class,java.lang.Class)
            systems.zlink.framework.configuration.ZLinkMeshChannelServerBuilder.addSendHandler(java.lang.Class,java.lang.Class)
            systems.zlink.framework.configuration.ZLinkMeshChannelServerBuilder.addRouteSendHandler(java.lang.Class,java.lang.Class)
            systems.zlink.framework.configuration.ZLinkMeshChannelServerBuilder.addRequestHandler(java.lang.Class,java.lang.Class,java.lang.Class)
            systems.zlink.framework.configuration.ZLinkClientServerChannelServerBuilder.addSendHandler(java.lang.Class,java.lang.Class)
            systems.zlink.framework.configuration.ZLinkClientServerChannelServerBuilder.addRequestHandler(java.lang.Class,java.lang.Class,java.lang.Class)
            systems.zlink.framework.spots.ZLinkSpotHandlerRegistry.addHandler(java.lang.Class)
            systems.zlink.framework.spots.ZLinkInstanceSpotHandlerRegistry.addPacket(java.lang.Class)
            systems.zlink.framework.spots.ZLinkEntrySpotContext.addTimer(java.lang.String,java.time.Duration,java.lang.Class,systems.zlink.framework.spots.ZLinkTimerOptions)
            systems.zlink.framework.spots.ZLinkSpotContext.addTimer(java.lang.String,java.time.Duration,java.lang.Class,systems.zlink.framework.spots.ZLinkTimerOptions)
            systems.zlink.framework.spots.ZLinkInstanceSpotContext.addTimer(java.lang.String,java.time.Duration,java.lang.Class,systems.zlink.framework.spots.ZLinkTimerOptions)
            systems.zlink.framework.channels.ZLinkSendCall.submit
            systems.zlink.framework.channels.ZLinkRequestCall.submit
            systems.zlink.framework.channels.ZLinkRequestCall.yield
            systems.zlink.framework.channels.ZLinkFanoutPublishCall.submit
            systems.zlink.framework.channels.ZLinkPublishCall.submit
            systems.zlink.framework.actors.ZLinkActorSendCall.submit
            systems.zlink.framework.actors.ZLinkActorRequestCall.submit
            systems.zlink.framework.actors.ZLinkActorRequestCall.yield
            systems.zlink.framework.actors.ZLinkActorCreateCall.submit
            systems.zlink.framework.actors.ZLinkActorCreateCall.yield
            systems.zlink.framework.actors.ZLinkActorGetOrCreateCall.submit
            systems.zlink.framework.actors.ZLinkActorGetOrCreateCall.yield
            systems.zlink.framework.actors.ZLinkBoundSessionSendCall.submit
            systems.zlink.framework.spots.ZLinkSpotSendCall.submit
            systems.zlink.framework.spots.ZLinkSpotRequestCall.submit
            systems.zlink.framework.spots.ZLinkSpotRequestCall.yield
            systems.zlink.framework.spots.ZLinkSpotCreateCall.submit
            systems.zlink.framework.spots.ZLinkSpotCreateCall.yield
            systems.zlink.framework.spots.ZLinkSpotGetOrCreateCall.submit
            systems.zlink.framework.spots.ZLinkSpotGetOrCreateCall.yield
            systems.zlink.framework.spots.ZLinkWorkerCall.submit
            systems.zlink.framework.spots.ZLinkWorkerCall.yield
            systems.zlink.framework.streams.ZLinkSessionSendCall.submit
            systems.zlink.framework.streams.ZLinkSessionReplyCall.submit
            """
                .trimIndent()
                .lines()
                .toSet()
        assertEquals(expectedMethods, configuredMethods)
        assertTrue(configuredMethods.none { it.endsWith("submit_sync") })
    }
}
