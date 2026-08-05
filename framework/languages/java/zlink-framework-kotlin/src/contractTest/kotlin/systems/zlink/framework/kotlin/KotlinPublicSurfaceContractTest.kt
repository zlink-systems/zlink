package systems.zlink.framework.kotlin

import java.lang.reflect.Modifier
import java.nio.file.Files
import java.nio.file.Path
import java.security.MessageDigest
import java.util.concurrent.CompletionStage
import kotlin.coroutines.Continuation
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertFalse
import org.junit.jupiter.api.Assertions.assertTrue
import org.junit.jupiter.api.Test
import systems.zlink.framework.channels.ZLinkClient
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.channels.ZLinkSendCall
import systems.zlink.framework.streams.ZLinkSessionActor

class KotlinPublicSurfaceContractTest {
    private val facadeClasses = listOf(
        "ZLinkConnectorExtensionsKt",
        "ZLinkCoroutineHandlerOptionsKt",
        "ZLinkCoroutineTurnAwaitKt",
        "ZLinkDispatchOptionsExtensionsKt",
        "ZLinkFrameworkExtensionsKt",
        "ZLinkLocationExtensionsKt",
        "ZLinkMessageExtensionsKt",
        "ZLinkSpotHandlerRegistryExtensionsKt",
    ).map { Class.forName("systems.zlink.framework.kotlin.$it") }

    @Test
    fun canonicalOneWayWrappersHideJavaCalls() {
        val wrapperTypes = listOf(
            ZLinkKotlinMessageSendCall::class.java,
            ZLinkKotlinRequestCall::class.java,
            ZLinkKotlinClient::class.java,
            ZLinkKotlinFanoutClient::class.java,
            ZLinkKotlinRouteClient::class.java,
            ZLinkKotlinActorClient::class.java,
            ZLinkKotlinActorCreateCall::class.java,
            ZLinkKotlinActorManager::class.java,
            ZLinkKotlinSpotCreateCall::class.java,
            ZLinkKotlinSpotManager::class.java,
            ZLinkKotlinSessionClient::class.java,
            ZLinkKotlinSessionSendCall::class.java,
            ZLinkKotlinSessionReplyCall::class.java,
            ZLinkKotlinWorkerCall::class.java,
        )
        wrapperTypes.flatMap { it.declaredMethods.asList() }.forEach { method ->
            assertFalse(
                method.returnType.name.startsWith("systems.zlink.framework.channels.ZLink"),
                "Java call return leaked from ${method.declaringClass.simpleName}.${method.name}",
            )
            assertFalse(
                method.parameterTypes.any {
                    it.name.startsWith("systems.zlink.framework.channels.ZLink")
                },
                "Java call parameter leaked from ${method.declaringClass.simpleName}.${method.name}",
            )
        }
    }

    @Test
    fun spotWrappersExposeCanonicalFluentState() {
        assertPublicMethodCounts(
            "ZLinkKotlinSpotSendCall",
            mapOf(
                "metadata" to 1,
                "instanceSpot" to 2,
                "inMesh" to 1,
                "await" to 1,
            ),
        )
        assertPublicMethodCounts(
            "ZLinkKotlinSpotRequestCall",
            mapOf(
                "metadata" to 1,
                "instanceSpot" to 2,
                "inMesh" to 1,
                "timeout" to 1,
                "await" to 1,
                "yield" to 1,
            ),
        )
        assertPublicMethodCounts(
            "ZLinkKotlinSpotCreateCall",
            mapOf(
                "inMesh" to 1,
                "request" to 1,
                "timeout" to 1,
                "await" to 1,
                "yield" to 1,
            ),
        )
        assertPublicMethodCounts(
            "ZLinkKotlinSpotManager",
            mapOf("create" to 1, "getOrCreate" to 1),
        )
    }

    @Test
    fun actorManagerWrapperExposesCanonicalFluentState() {
        assertPublicMethodCounts(
            "ZLinkKotlinActorCreateCall",
            mapOf(
                "inMesh" to 1,
                "request" to 1,
                "timeout" to 1,
                "await" to 1,
                "yield" to 1,
            ),
        )
        assertPublicMethodCounts(
            "ZLinkKotlinActorManager",
            mapOf("create" to 1, "getOrCreate" to 1),
        )
    }

    @Test
    fun `one way projections preserve call and completion types`() {
        assertTrue(
            ZLinkKotlinClient::class.java.methods.any {
                it.name == "sendToChannel" &&
                    it.returnType == ZLinkKotlinMessageSendCall::class.java
            },
        )
        assertTrue(
            ZLinkKotlinRouteClient::class.java.methods.count {
                it.name.startsWith("sendTo") &&
                    it.returnType == ZLinkKotlinMessageSendCall::class.java
            } >= 2,
        )
        // kotlin/interfaces/stream-session.ko.md:83-84 projects both relay overloads onto
        // ZLinkKotlinSubmissionCall, not onto the channel send call.
        assertEquals(
            2,
            ZLinkKotlinSessionActor::class.java.methods.count {
                it.name == "relay" &&
                    it.returnType == ZLinkKotlinSubmissionCall::class.java
            },
        )
    }

    @Test
    fun `one way coroutine surface exposes await and no yield alternative`() {
        val oneWayTypes = listOf(
            ZLinkKotlinMessageSendCall::class.java,
            ZLinkKotlinSessionSendCall::class.java,
            ZLinkKotlinSessionReplyCall::class.java,
        )
        assertTrue(oneWayTypes.all { type -> type.declaredMethods.any { method ->
            method.name == "await" &&
                method.parameterTypes.lastOrNull() == Continuation::class.java
        } })
        assertFalse(oneWayTypes.any { type ->
            type.declaredMethods.any { it.name.contains("yield", ignoreCase = true) }
        })

        val surfaceClasses = listOf(
            Class.forName("systems.zlink.framework.kotlin.ZLinkCoroutineTurnAwaitKt"),
            Class.forName("systems.zlink.framework.kotlin.ZLinkFrameworkExtensionsKt"),
            Class.forName("systems.zlink.framework.kotlin.ZLinkConnectorExtensionsKt"),
            Class.forName("systems.zlink.framework.kotlin.ZLinkOneWayCallsKt"),
        )
        val publicMethods = surfaceClasses
            .flatMap { it.declaredMethods.asList() }
            .filter { Modifier.isPublic(it.modifiers) }

        assertFalse(publicMethods.any { method ->
            method.parameterTypes.any { it.simpleName.contains("CancellationToken") }
        })
    }

    @Test
    fun `typed and raw connector requests have coroutine terminators without signature clashes`() {
        val methods = Class.forName("systems.zlink.framework.kotlin.ZLinkConnectorExtensionsKt")
            .declaredMethods
            .filter { Modifier.isPublic(it.modifiers) }

        val signatures = methods.map { method ->
            method.name to method.parameterTypes.toList()
        }
        assertEqualsDistinct(signatures)
        // The typed terminator is awaitReply and the raw terminator is await. Keeping a typed
        // await() overload is what produced the JvmName("awaitTyped") clash, so the raw name
        // must never carry a typed receiver.
        assertEquals(2, methods.count { it.name == "awaitReply" })
        assertEquals(2, methods.count { it.name == "await" })
        assertFalse(methods.any { it.name == "awaitTyped" })
        assertFalse(methods.any { method ->
            method.name == "await" &&
                method.parameterTypes.first().simpleName == "ZLinkTypedStreamRequestCall"
        })
    }

    @Test
    fun `documented Kotlin types and function names remain public`() {
        val expectedTypes = setOf(
            "ZLinkSuspendingRequestHandler",
            "ZLinkSuspendingSendHandler",
            "ZLinkSuspendingPublishHandler",
            "ZLinkSuspendingRouteRequestHandler",
            "ZLinkSuspendingRouteSendHandler",
            "ZLinkSuspendingSpotPacketHandler",
            "ZLinkSuspendingSpotRequestHandler",
            "ZLinkSuspendingSpotSubscriptionHandler",
            "ZLinkSuspendingSpotTimerHandler",
            "ZLinkSuspendingEntrySpotActorSendHandler",
            "ZLinkSuspendingEntrySpotActorRequestHandler",
            "ZLinkSuspendingSpotActorSendHandler",
            "ZLinkSuspendingSpotActorRequestHandler",
            "ZLinkSuspendingTypedSessionPacketHandler",
            "ZLinkSuspendingActorFactory",
            "ZLinkSuspendingSpot",
            "ZLinkSuspendingEntrySpot",
            "ZLinkSuspendingInstanceSpot",
            "ZLinkSuspendingSession",
            "ZLinkCoroutineSuspendHandlerInvoker",
            "ZLinkKotlinLifecycleCall",
            "ZLinkKotlinSendCall",
            "ZLinkKotlinActorCreateCall",
            "ZLinkKotlinActorManager",
            "ZLinkKotlinSpotCreateCall",
            "ZLinkKotlinSpotManager",
            "ZLinkKotlinStreamConnector",
            "ZLinkStreamTypedWaitCall",
        )
        expectedTypes.forEach { Class.forName("systems.zlink.framework.kotlin.$it") }

        val publicFunctionNames = facadeClasses
            .flatMap { it.declaredMethods.asList() }
            .filter { Modifier.isPublic(it.modifiers) }
            .map { it.name }
            .toSet()
        val expectedFunctionNames = setOf(
            "actorFactory", "actorRef", "addHandler", "asFlow", "await", "awaitReply",
            "bindOrGetActor",
            "configureStreamCompression", "configureDispatch", "decode",
            "ensureActor", "errors", "findActor", "isPeerReady", "kotlin",
            "listServiceSummaries", "listTopology",
            "messageOf", "messages", "onMessageFlow", "publishToTopic",
            "request", "requestToActorAwait",
            "send", "snapshot", "status", "topology", "useCoroutineHandlers",
            "waitFor", "withDefaultStreamCompression", "withLz4StreamCompression",
            "withStreamCompression", "withoutStreamCompression",
        )
        assertTrue(
            publicFunctionNames.containsAll(expectedFunctionNames),
            "missing Kotlin public functions: ${expectedFunctionNames - publicFunctionNames}",
        )
    }

    @Test
    fun `Kotlin application contracts have one source owner without changing FQNs`() {
        val sourceRoot = Path.of("src/main/kotlin/systems/zlink/framework/kotlin")
        val contractRoot = sourceRoot.resolve("contracts")
        assertTrue(Files.isRegularFile(contractRoot.resolve("ZLinkOneWayContracts.kt")))
        assertTrue(Files.isRegularFile(contractRoot.resolve("ZLinkSuspendingHandlers.kt")))
        assertFalse(Files.exists(sourceRoot.resolve("ZLinkSuspendingHandlers.kt")))

        val runtimeSource = Files.readString(sourceRoot.resolve("ZLinkOneWayCalls.kt"))
        assertFalse(
            Regex("(?m)^interface\\s+ZLinkKotlin").containsMatchIn(runtimeSource),
            "Kotlin call contracts must be declared under the contracts source owner",
        )

        // Moving source ownership must not rename the public package.
        assertEquals(
            "systems.zlink.framework.kotlin",
            ZLinkKotlinMessageSendCall::class.java.packageName,
        )
        assertEquals(
            "systems.zlink.framework.kotlin",
            ZLinkSuspendingInstanceSpot::class.java.packageName,
        )
    }

    @Test
    fun `suspending Spot exposes relocation readiness completion bridge`() {
        val type = ZLinkSuspendingSpot::class.java
        val suspending = type.declaredMethods.single {
            it.name == "onRelocationReadyCompletedSuspending"
        }
        assertTrue(Modifier.isProtected(suspending.modifiers))
        assertEquals(
            systems.zlink.framework.spots
                .ZLinkSpotRelocationReadyCompletion::class.java,
            suspending.parameterTypes.first(),
        )
        val bridge = type.getMethod(
            "onRelocationReadyCompleted",
            systems.zlink.framework.spots
                .ZLinkSpotRelocationReadyCompletion::class.java,
        )
        assertEquals(CompletionStage::class.java, bridge.returnType)
    }

    @Test
    fun `suspending Spot lifecycle matches the exact interface`() {
        assertSuspendingSpotLifecycle(
            ZLinkSuspendingSpot::class.java,
            systems.zlink.framework.spots.ZLinkSpotContext::class.java,
        )
        assertSuspendingSpotLifecycle(
            ZLinkSuspendingEntrySpot::class.java,
            systems.zlink.framework.spots.ZLinkEntrySpotContext::class.java,
        )

        val timerSpotBound = ZLinkSuspendingSpotTimerHandler::class.java
            .typeParameters.single().bounds.single()
        assertEquals(Any::class.java, timerSpotBound)
    }

    private fun assertSuspendingSpotLifecycle(type: Class<*>, contextType: Class<*>) {
        val contextGetter = type.getDeclaredMethod("getContext")
        assertTrue(Modifier.isAbstract(contextGetter.modifiers))
        assertEquals(contextType, contextGetter.returnType)

        val contextBridge = type.getDeclaredMethod("context")
        assertTrue(Modifier.isFinal(contextBridge.modifiers))
        assertEquals(contextType, contextBridge.returnType)

        val closingBridge = type.getDeclaredMethod(
            "onClosing",
            systems.zlink.framework.spots.ZLinkSpotClosingContext::class.java,
        )
        assertTrue(Modifier.isFinal(closingBridge.modifiers))
        assertEquals(CompletionStage::class.java, closingBridge.returnType)
        assertFalse(type.declaredMethods.any {
            it.name == "onClosing" && it.parameterCount == 0
        })

        val suspending = type.getDeclaredMethod(
            "onClosingSuspending",
            systems.zlink.framework.spots.ZLinkSpotClosingContext::class.java,
            Continuation::class.java,
        )
        assertTrue(Modifier.isProtected(suspending.modifiers))
    }

    @Test
    fun `documented top level extension overloads remain fixed`() {
        assertFacadeMethodCounts(
            "ZLinkConnectorExtensionsKt",
            mapOf(
                "kotlin" to 1, "withDefaultStreamCompression" to 1,
                "withLz4StreamCompression" to 1, "withStreamCompression" to 1,
                "withoutStreamCompression" to 1, "await" to 2,
                "awaitReply" to 2, "waitFor" to 1,
                "messages" to 1, "errors" to 1,
            ),
        )
        assertFacadeMethodCounts(
            "ZLinkCoroutineHandlerOptionsKt",
            mapOf("useCoroutineHandlers" to 2),
        )
        assertFacadeMethodCounts("ZLinkCoroutineTurnAwaitKt", mapOf("await" to 1))
        assertFacadeMethodCounts(
            "ZLinkDispatchOptionsExtensionsKt",
            mapOf("configureDispatch" to 1, "onMessageFlow" to 1),
        )
        assertFacadeMethodCounts(
            "ZLinkFrameworkExtensionsKt",
            mapOf(
                "awaitReply" to 4, "yieldReply" to 4, "yieldWorker" to 1,
                "requestToActorAwait" to 2,
                "findActor" to 1, "ensureActor" to 2, "snapshot" to 1,
                "actorRef" to 1, "isPeerReady" to 1, "bindOrGetActor" to 1,
                "send" to 3, "request" to 2,
                "sendToSpotCall" to 1, "requestToSpotAwait" to 1,
                "publishToTopic" to 1,
                "configureStreamCompression" to 1,
                "actorFactory" to 1,
            ),
        )
        assertFacadeMethodCounts(
            "ZLinkLocationExtensionsKt",
            mapOf(
                "topology" to 1,
                "status" to 1,
                "listTopology" to 1,
                "listServiceSummaries" to 1,
                "asFlow" to 1,
            ),
        )
        assertFacadeMethodCounts(
            "ZLinkMessageExtensionsKt",
            mapOf("messageOf" to 1, "decode" to 1),
        )
        assertFacadeMethodCounts(
            "ZLinkSpotHandlerRegistryExtensionsKt",
            mapOf("addHandler" to 1, "addTypedHandler" to 1),
        )
    }

    @Test
    fun `one way Kotlin calls expose only await coroutine completion`() {
        val oneWayTypes = listOf(
            ZLinkKotlinMessageSendCall::class.java,
            ZLinkKotlinSessionSendCall::class.java,
            ZLinkKotlinSessionReplyCall::class.java,
        )
        oneWayTypes.forEach { type ->
            val methods = type.declaredMethods.filter { Modifier.isPublic(it.modifiers) }
            assertEquals(1, methods.count { it.name == "await" })
            assertFalse(methods.any { it.name.contains("yield", ignoreCase = true) })
            assertFalse(methods.any { method ->
                method.returnType == CompletionStage::class.java ||
                    method.parameterTypes.any { it == CompletionStage::class.java }
            })
        }

        val typedWaitMethods = Class.forName(
            "systems.zlink.framework.kotlin.ZLinkStreamTypedWaitCall",
        ).declaredMethods.filter { Modifier.isPublic(it.modifiers) }
        assertFalse(typedWaitMethods.any { it.name == "submit" })
    }

    @Test
    fun `Kotlin connector wrappers expose exactly the documented method groups`() {
        assertPublicMethodCounts(
            "ZLinkKotlinStreamConnector",
            mapOf(
                "isConnected" to 1, "getState" to 1, "getOptions" to 1,
                "getPendingDispatchCount" to 1, "receivedCount" to 1,
                "observeInbound" to 1, "on" to 2, "onErrorReceived" to 1,
                "onDisconnected" to 1, "onConnectionStateChanged" to 1,
                "connect" to 1,
                "close" to 1, "dispatch" to 1, "send" to 2, "request" to 2,
                // stream-connector/languages/java/03-stream-connector.ko.md:460-463 declares
                // waitFor, expectNone and waitForSequence on the Kotlin connector.
                "waitFor" to 2, "expectNone" to 1, "waitForSequence" to 1,
                "messages" to 1, "errors" to 1,
            ),
        )
        assertPublicMethodCounts("ZLinkKotlinLifecycleCall", mapOf("await" to 1))
        assertPublicMethodCounts("ZLinkKotlinSendCall", mapOf("await" to 1))
        assertPublicMethodCounts(
            "ZLinkStreamTypedWaitCall",
            mapOf("timeout" to 1, "where" to 1, "await" to 1),
        )
        assertPublicMethodCounts(
            "ZLinkStreamTypedExpectNoneCall",
            mapOf("within" to 1, "await" to 1),
        )
        assertPublicMethodCounts(
            "ZLinkStreamTypedSequenceCall",
            mapOf("expect" to 1, "timeout" to 1, "await" to 1),
        )
    }

    @Test
    fun `RouteMesh runtime options use the contracted Java projection`() {
        val type = Class.forName(
            "systems.zlink.framework.channels.ZLinkRouteMeshRuntimeOptions",
        )
        assertEquals(
            "systems.zlink.framework.channels.ZLinkMeshNodeRuntimeOptions",
            type.getMethod("meshNode", String::class.java).returnType.name,
        )
        assertEquals(
            "systems.zlink.framework.channels.ZLinkMeshChannelRuntimeOptions",
            type.getMethod("channel", String::class.java, String::class.java)
                .returnType.name,
        )
        assertEquals(
            "systems.zlink.framework.channels.ZLinkMeshPlacementRuntimeOptions",
            type.getMethod("mesh", String::class.java).returnType.name,
        )
        assertEquals(
            "systems.zlink.framework.channels.ZLinkMeshChannelRuntimeOptions",
            type.getMethod("channel", String::class.java).returnType.name,
        )
    }

    @Test
    fun `documented Kotlin APIs retain their exact JVM descriptors`() {
        val expectedHashes = mapOf(
            "ZLinkConnectorExtensionsKt" to "a83aa6550a9fd806bd21496bfd1eafac476f4ae368cd99fb624b22bc1931a76f",
            "ZLinkCoroutineHandlerOptionsKt" to "67fda6a26015bcd374098db883ec13f012b2536da914e6b3e8fb0f6aea9e86f4",
            "ZLinkCoroutineTurnAwaitKt" to "0e58ca9d82f2e14d26e4763e955296534d7ce8e863c8fdd5b5231148a5661d5f",
            "ZLinkDispatchOptionsExtensionsKt" to "eb26c767da350a140c90c974e5485a3ba7af9265bb0f6badec8a381efb591443",
            "ZLinkFrameworkExtensionsKt" to "269802638a1b53095e969d738fbf4789b295cc3e388e03729f82f7523e5b5bfb",
            "ZLinkLocationExtensionsKt" to "81932a1fe63f6a00014d4eab8258c4e905529694e94bfa0e64ee8293d03e2100",
            "ZLinkMessageExtensionsKt" to "836b0c8038be8ee1beae9f8cf1f59cbd7e0811e936d1a4d47e7625b37abdaa9e",
            "ZLinkSpotHandlerRegistryExtensionsKt" to "0cc8a319eb99070b97332cab96c480fc74c14b9b160b022fa8d60ab4de814196",
            "ZLinkKotlinStreamConnector" to "eaf3cd7485e7e0a2bbf9d4db7ff5268422637947f00718083684002034c00993",
            "ZLinkKotlinLifecycleCall" to "bef9eb581a23386b7802f54c64e3fec57c9920a17745c00c59195f7e67949aa5",
            "ZLinkKotlinSendCall" to "bef9eb581a23386b7802f54c64e3fec57c9920a17745c00c59195f7e67949aa5",
            "ZLinkStreamTypedWaitCall" to "6385a73bc528712e6d0f31512ba8f29c1951b2c347c48b6001f03c34e80d84f4",
        )
        // Report every drifted type at once. Failing on the first one left the remaining
        // goldens unverified for as long as the first entry stayed red.
        val drifted = expectedHashes.mapNotNull { (typeName, expectedHash) ->
            val signatures = publicJvmSignatures(typeName)
            val actualHash = MessageDigest.getInstance("SHA-256")
                .digest((signatures.joinToString("\n") + "\n").toByteArray())
                .joinToString("") { byte -> "%02x".format(byte) }
            if (actualHash == expectedHash) {
                null
            } else {
                "$typeName expected=$expectedHash actual=$actualHash signatures=$signatures"
            }
        }
        assertTrue(drifted.isEmpty(), "Kotlin JVM descriptors changed:\n${drifted.joinToString("\n")}")
    }

    private fun publicJvmSignatures(typeName: String): List<String> =
        Class.forName("systems.zlink.framework.kotlin.$typeName")
            .declaredMethods
            .filter { method ->
                Modifier.isPublic(method.modifiers) &&
                    method.name != "awaitFrameworkStage" &&
                    method.name != "locationPages" &&
                    !method.name.endsWith("\$default") &&
                    !method.name.startsWith("access\$") &&
                    !method.name.startsWith("getInner")
            }
            .map { method ->
                method.name + " " + method.parameterTypes.joinToString(
                    separator = "",
                    prefix = "(",
                    postfix = ")",
                ) { jvmDescriptor(it) } + jvmDescriptor(method.returnType)
            }
            .sorted()

    private fun jvmDescriptor(type: Class<*>): String = when {
        type.isArray -> type.name.replace('.', '/')
        !type.isPrimitive -> "L${type.name.replace('.', '/')};"
        type == Void.TYPE -> "V"
        type == Boolean::class.javaPrimitiveType -> "Z"
        type == Byte::class.javaPrimitiveType -> "B"
        type == Char::class.javaPrimitiveType -> "C"
        type == Short::class.javaPrimitiveType -> "S"
        type == Int::class.javaPrimitiveType -> "I"
        type == Long::class.javaPrimitiveType -> "J"
        type == Float::class.javaPrimitiveType -> "F"
        type == Double::class.javaPrimitiveType -> "D"
        else -> error("unsupported primitive JVM type: $type")
    }

    private fun assertPublicMethodCounts(typeName: String, expected: Map<String, Int>) {
        val actual = Class.forName("systems.zlink.framework.kotlin.$typeName")
            .declaredMethods
            .filter { Modifier.isPublic(it.modifiers) && !it.name.startsWith("getInner") }
            .groupingBy { it.name }
            .eachCount()
        assertEquals(expected, actual, "$typeName public method overloads changed")
    }

    private fun assertFacadeMethodCounts(typeName: String, expected: Map<String, Int>) {
        val ignoredNames = setOf("awaitFrameworkStage", "locationPages")
        val actual = Class.forName("systems.zlink.framework.kotlin.$typeName")
            .declaredMethods
            .filter { method ->
                Modifier.isPublic(method.modifiers) &&
                    method.name !in ignoredNames &&
                    !method.name.endsWith("\$default") &&
                    !method.name.startsWith("access\$")
            }
            .groupingBy { it.name }
            .eachCount()
        assertEquals(expected, actual, "$typeName public extension overloads changed")
    }

    private fun assertExtensionReturnCount(
        receiver: Class<*>,
        methodName: String,
        returnType: Class<*>,
        expected: Int,
    ) {
        val methods = Class.forName("systems.zlink.framework.kotlin.ZLinkFrameworkExtensionsKt")
            .declaredMethods
            .filter { method ->
                Modifier.isPublic(method.modifiers) &&
                    method.name == methodName &&
                    method.parameterTypes.firstOrNull() == receiver &&
                    method.returnType == returnType
            }
        assertEquals(expected, methods.size)
    }

    private fun assertStageResultType(
        owner: Class<*>,
        methodName: String,
        resultType: Class<*>,
    ) {
        val methods = owner.methods.filter { it.name == methodName }
        assertTrue(methods.isNotEmpty())
        methods.forEach { method ->
            assertTrue(method.genericReturnType.typeName.contains(resultType.name))
        }
    }

    private fun assertEqualsDistinct(signatures: List<Pair<String, List<Class<*>>>>) {
        assertTrue(signatures.size == signatures.distinct().size)
    }
}
