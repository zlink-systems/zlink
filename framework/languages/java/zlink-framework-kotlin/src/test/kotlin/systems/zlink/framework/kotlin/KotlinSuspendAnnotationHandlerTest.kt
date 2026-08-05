package systems.zlink.framework.kotlin

import java.time.Duration
import java.util.concurrent.CancellationException
import java.util.concurrent.CompletableFuture
import java.util.concurrent.ExecutionException
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference
import kotlin.coroutines.coroutineContext
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.asCoroutineDispatcher
import kotlinx.coroutines.delay
import kotlinx.coroutines.Job
import kotlinx.coroutines.withContext
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertSame
import org.junit.jupiter.api.Assertions.assertTrue
import org.junit.jupiter.api.Test
import org.junit.jupiter.api.assertThrows
import org.springframework.context.annotation.AnnotationConfigApplicationContext
import org.springframework.context.annotation.Bean
import org.springframework.context.annotation.Configuration
import systems.zlink.contracts.core.RoutingId
import systems.zlink.framework.channels.ZLinkPublishMessageContext
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.actors.ZLinkActorContext
import systems.zlink.framework.actors.ZLinkActorFactory
import systems.zlink.framework.errors.ZLinkConfigurationException
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.handlers.ZLinkPacket
import systems.zlink.framework.handlers.ZLinkPublish
import systems.zlink.framework.handlers.ZLinkRequest
import systems.zlink.framework.handlers.ZLinkSend
import systems.zlink.framework.handlers.ZLinkSpotActorRequest
import systems.zlink.framework.handlers.ZLinkSpotActorSend
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker
import systems.zlink.framework.runtime.handlers.ZLinkHandlerScanner
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerKind
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerSurface
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkAutoConfiguration
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue
import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle
import systems.zlink.framework.spots.ZLinkSpot
import systems.zlink.framework.spots.ZLinkSpotContext
import systems.zlink.framework.testkit.FakeZLinkBackendAdapterFactory

final class KotlinSuspendAnnotationHandlerTest {
    @Test
    fun clientServerRoleBuildersAllowSameAndDifferentChannelNames() {
        val options = DefaultZLinkFrameworkOptions()

        options.addClientServerChannel("orders").client()
            .connect("inproc://orders")
        options.addClientServerChannel("orders").server().listen()
        options.addClientServerChannel("billing").client()
            .connect("inproc://billing")
        options.addClientServerChannel("inventory").server().listen()

        assertTrue(true)
    }

    @Test
    fun scannerTreatsKotlinSuspendChannelAnnotationsLikeJavaMethodHandlers() {
        val catalog = ZLinkHandlerScanner.scan(setOf(KotlinSuspendHandlerMarker::class.java))

        val request = catalog.matching(
            setOf("kotlin-channel"),
            ZLinkScannedHandlerSurface.CHANNEL,
            ZLinkScannedHandlerKind.REQUEST,
        ).single()
        val send = catalog.matching(
            setOf("kotlin-channel"),
            ZLinkScannedHandlerSurface.CHANNEL,
            ZLinkScannedHandlerKind.SEND,
        ).single()
        val publish = catalog.matching(
            setOf("kotlin-fanout"),
            ZLinkScannedHandlerSurface.CHANNEL,
            ZLinkScannedHandlerKind.PUBLISH,
        ).single()

        assertEquals(ProfileRequest::class.java, request.messageType())
        assertEquals(ProfileReply::class.java, request.replyType())
        assertEquals(ProfileGreeting::class.java, send.messageType())
        assertEquals(ProfileEvent::class.java, publish.messageType())
        assertTrue(ZLinkHandlerMethodInvoker.isKotlinSuspendMethod(request.handlerMethod()))
    }

    @Test
    fun scannerTreatsKotlinSuspendingInterfacesLikeFirstClassHandlers() {
        val catalog = ZLinkHandlerScanner.scan(setOf(KotlinSuspendHandlerMarker::class.java))

        val request = catalog.matching(
            setOf("kotlin-interface-channel"),
            ZLinkScannedHandlerSurface.CHANNEL,
            ZLinkScannedHandlerKind.REQUEST,
        ).single()
        val timer = catalog.matching(
            setOf("kotlin-interface-spot"),
            ZLinkScannedHandlerSurface.SPOT,
            ZLinkScannedHandlerKind.TIMER,
        ).single()

        assertEquals(ProfileRequest::class.java, request.messageType())
        assertEquals(ProfileReply::class.java, request.replyType())
        assertEquals(InterfaceSpot::class.java, timer.spotType())
        assertEquals("interface-timer", timer.timerName())
        val requestMethod = ZLinkHandlerMethodInvoker.requireHandlerMethod(
            request.handlerType(),
            "handle",
            arrayOf(ProfileRequest("Ada"), requestContext("profile")),
        )
        val timerMethod = ZLinkHandlerMethodInvoker.requireHandlerMethod(
            timer.handlerType(),
            "handle",
            arrayOfNulls<Any>(2),
        )
        assertTrue(ZLinkHandlerMethodInvoker.isKotlinSuspendMethod(requestMethod))
        assertTrue(ZLinkHandlerMethodInvoker.isKotlinSuspendMethod(timerMethod))
    }

    @Test
    fun scannerKeepsMultipleKotlinSuspendingEntrySpotActorRequestHandlers() {
        val catalog = ZLinkHandlerScanner.scan(setOf(KotlinSuspendHandlerMarker::class.java))

        val requests = catalog.matching(
            setOf("kotlin-interface-spot"),
            ZLinkScannedHandlerSurface.SPOT,
            ZLinkScannedHandlerKind.ACTOR_REQUEST,
        )
        val packetNames = requests.map { it.packetName() }.toSet()

        assertTrue(packetNames.contains("InterfacePlayerCommand"))
        assertTrue(packetNames.contains("SecondInterfacePlayerCommand"))
        assertTrue(requests.any { it.replyType() == InterfacePlayerReply::class.java })
        assertTrue(requests.any { it.replyType() == SecondInterfacePlayerReply::class.java })
    }

    @Test
    fun kotlinSuspendingInterfaceHandlerRunsThroughMethodInvoker() {
        val handler = KotlinSuspendingInterfaceRequestHandler()

        val reply = ZLinkHandlerMethodInvoker
            .invokeHandler(
                handler,
                "handle",
                arrayOf(ProfileRequest("Ada"), requestContext("profile")),
                listOf(ZLinkCoroutineSuspendHandlerInvoker()),
            )
            .toCompletableFuture()
            .get(1, TimeUnit.SECONDS)

        assertEquals(ProfileReply("profile:Ada"), reply)
    }

    @Test
    fun kotlinSuspendInvocationFailsClearlyWhenNoProviderSupportsTheMethod() {
        val handler = KotlinSuspendingInterfaceRequestHandler()
        val method = ZLinkHandlerMethodInvoker.requireHandlerMethod(
            handler.javaClass,
            "handle",
            arrayOf(ProfileRequest("Ada"), requestContext("profile")),
        )
        val unsupportedInvoker = object : ZLinkSuspendInvocationAdapter {
            override fun supports(method: java.lang.reflect.Method): Boolean = false

            override fun invoke(
                handler: Any,
                method: java.lang.reflect.Method,
                logicalArguments: Array<Any>,
            ) = CompletableFuture.failedFuture<Any>(AssertionError("unsupported invoker must not run"))
        }

        val failure = assertThrows<ExecutionException> {
            ZLinkHandlerMethodInvoker
                .invoke(
                    handler,
                    method,
                    arrayOf(ProfileRequest("Ada"), requestContext("profile")),
                    listOf(unsupportedInvoker),
                )
                .toCompletableFuture()
                .get(1, TimeUnit.SECONDS)
        }

        assertTrue(failure.cause is ZLinkConfigurationException)
        assertTrue(failure.cause!!.message!!.contains("registered ZLinkSuspendInvocationAdapter"))
    }

    @Test
    fun nonSuspendCompletionStageHandlerResultIsFlattened() {
        val handler = CompletionStageHandler()
        val method = handler.javaClass.getMethod("handle", ProfileRequest::class.java)

        val reply = ZLinkHandlerMethodInvoker
            .invoke(handler, method, arrayOf(ProfileRequest("Ada")))
            .toCompletableFuture()
            .get(1, TimeUnit.SECONDS)

        assertEquals(ProfileReply("stage:Ada"), reply)
        assertTrue(reply !is CompletableFuture<*>)
    }

    @Test
    fun scannerTreatsKotlinSuspendSpotActorAnnotationsLikeJavaMethodHandlers() {
        val catalog = ZLinkHandlerScanner.scan(setOf(KotlinSuspendHandlerMarker::class.java))

        val request = catalog.matching(
            setOf("kotlin-spot"),
            ZLinkScannedHandlerSurface.SPOT,
            ZLinkScannedHandlerKind.ACTOR_REQUEST,
        ).single()
        val send = catalog.matching(
            setOf("kotlin-spot"),
            ZLinkScannedHandlerSurface.SPOT,
            ZLinkScannedHandlerKind.ACTOR_SEND,
        ).single()

        assertEquals(PlayerCommand::class.java, request.messageType())
        assertEquals(PlayerReply::class.java, request.replyType())
        assertEquals(PlayerEvent::class.java, send.messageType())
        assertTrue(catalog.handlers().none {
            it.kind() == ZLinkScannedHandlerKind.ACTOR_JOIN ||
                it.kind() == ZLinkScannedHandlerKind.ACTOR_JOINED ||
                it.kind() == ZLinkScannedHandlerKind.ACTOR_LEFT
        })
        assertTrue(ZLinkHandlerMethodInvoker.isKotlinSuspendMethod(request.handlerMethod()))
    }

    @Test
    fun kotlinSuspendSpotActorMethodRunsThroughMethodInvoker() {
        val handler = KotlinSpringSuspendSpotActorHandler()
        val method = KotlinSpringSuspendSpotActorHandler::class.java.methods.single {
            it.name == "request"
        }

        val reply = ZLinkHandlerMethodInvoker
            .invoke(
                handler,
                method,
                arrayOf(PlayerActor("p1"), PlayerCommand("move")),
            )
            .toCompletableFuture()
            .get(1, TimeUnit.SECONDS)

        assertEquals(PlayerReply("p1:move"), reply)
    }

    @Test
    fun kotlinSuspendAnnotationRunsInsideFrameworkCoroutineContext() {
        val handler = KotlinCoroutineContextHandler()
        val method = KotlinCoroutineContextHandler::class.java.methods.single {
            it.name == "request"
        }

        val reply = ZLinkHandlerMethodInvoker
            .invoke(
                handler,
                method,
                arrayOf(ProfileRequest("Ada")),
            )
            .toCompletableFuture()
            .get(1, TimeUnit.SECONDS)

        assertEquals(ProfileReply("coroutine:Ada"), reply)
    }

    @Test
    fun kotlinSuspendHandlerKeepsYieldTurnAfterDispatcherSwitch() {
        val firstExecutor = Executors.newSingleThreadExecutor { task ->
            Thread(task, "zlink-kotlin-first-dispatcher").apply { isDaemon = true }
        }
        val secondExecutor = Executors.newSingleThreadExecutor { task ->
            Thread(task, "zlink-kotlin-second-dispatcher").apply { isDaemon = true }
        }
        firstExecutor.asCoroutineDispatcher().use { firstDispatcher ->
            secondExecutor.asCoroutineDispatcher().use { secondDispatcher ->
                val replyStage = CompletableFuture.completedFuture(ProfileReply("yield:Ada"))
                val handler = KotlinYieldAfterDispatcherSwitchHandler(secondDispatcher, replyStage)
                val method = KotlinYieldAfterDispatcherSwitchHandler::class.java.methods.single {
                    it.name == "request"
                }
                val result = CompletableFuture<Any?>()
                val queue = ZLinkAsyncSerialQueue()

                queue.enqueue {
                    ZLinkHandlerMethodInvoker
                        .invoke(
                            handler,
                            method,
                            arrayOf(ProfileRequest("Ada")),
                            listOf(ZLinkCoroutineSuspendHandlerInvoker(firstDispatcher)),
                        )
                        .whenComplete { reply, error ->
                            if (error == null) {
                                result.complete(reply)
                            } else {
                                result.completeExceptionally(error)
                            }
                        }
                        .thenApply {
                            null
                        }
                }

                assertEquals(ProfileReply("yield:Ada"), result.get(3, TimeUnit.SECONDS))
            }
        }
        firstExecutor.shutdownNow()
        secondExecutor.shutdownNow()
    }

    @Test
    fun frameworkOptionsConfigureKotlinSuspendDispatcher() {
        val executor = Executors.newSingleThreadExecutor { task ->
            Thread(task, "zlink-kotlin-handler-dispatcher").apply { isDaemon = true }
        }
        executor.asCoroutineDispatcher().use { dispatcher ->
            val options = DefaultZLinkFrameworkOptions()
            options.useCoroutineHandlers(dispatcher)
            val invokers = options.registration().suspendHandlerInvokers()
            val handler = KotlinDispatcherObservationHandler()
            val method = KotlinDispatcherObservationHandler::class.java.methods.single {
                it.name == "request"
            }

            val reply = ZLinkHandlerMethodInvoker
                .invoke(
                    handler,
                    method,
                    arrayOf(ProfileRequest("Ada")),
                    invokers,
                )
                .toCompletableFuture()
                .get(1, TimeUnit.SECONDS)

            assertEquals(ProfileReply("Ada"), reply)
            assertTrue(handler.threadName.get().startsWith("zlink-kotlin-handler-dispatcher"))
            assertTrue(handler.hasCoroutineJob.get())
        }
        executor.shutdownNow()
    }

    @Test
    fun springCreatedKotlinSuspendHandlerRunsThroughMethodInvoker() {
        AnnotationConfigApplicationContext().use { context ->
            context.register(SpringSuspendHandlerConfig::class.java)
            context.refresh()
            val handler = context.getBean(KotlinSpringSuspendChannelHandler::class.java)
            val method = KotlinSpringSuspendChannelHandler::class.java.methods.single {
                it.name == "request"
            }

            val reply = ZLinkHandlerMethodInvoker
                .invoke(
                    handler,
                    method,
                    arrayOf(ProfileRequest("Ada"), requestContext("profile")),
                )
                .toCompletableFuture()
                .get(1, TimeUnit.SECONDS)

            assertEquals(ProfileReply("profile:Ada:injected"), reply)
            assertSame(handler, context.getBean(KotlinSpringSuspendChannelHandler::class.java))
        }
    }

    @Test
    fun kotlinSuspendAnnotationExceptionCompletesJavaStageExceptionally() {
        val handler = KotlinSuspendFailureHandler()
        val method = KotlinSuspendFailureHandler::class.java.methods.single {
            it.name == "fail"
        }

        val failure = assertThrows<ExecutionException> {
            ZLinkHandlerMethodInvoker
                .invoke(handler, method, arrayOf(ProfileRequest("Ada")))
                .toCompletableFuture()
                .get(1, TimeUnit.SECONDS)
        }

        assertTrue(
            failure.cause is IllegalStateException,
            "unexpected cause: ${failure.cause?.javaClass?.name}: ${failure.cause?.message}",
        )
        assertEquals("boom:Ada", failure.cause!!.message)
    }

    @Test
    fun kotlinSuspendAnnotationCancellationCompletesJavaStageExceptionally() {
        val handler = KotlinSuspendFailureHandler()
        val method = KotlinSuspendFailureHandler::class.java.methods.single {
            it.name == "cancel"
        }

        val failure = assertThrows<CancellationException> {
            ZLinkHandlerMethodInvoker
                .invoke(handler, method, arrayOf(ProfileRequest("Ada")))
                .toCompletableFuture()
                .get(1, TimeUnit.SECONDS)
        }

        assertEquals("cancel:Ada", failure.message)
    }

    @Test
    fun duplicateValidationRejectsJavaAndKotlinSuspendAnnotationPacketCollision() {
        val options = DefaultZLinkFrameworkOptions()
        options.addHandlersFromPackageOf(KotlinSuspendHandlerMarker::class.java)
        val channel = options.addClientServerChannel("profile").server().listen()
        channel.addHandlerGroup("kotlin-channel")
        channel.addRequestHandler(
            JavaProfileRequestHandler::class.java,
            ProfileRequest::class.java,
            ProfileReply::class.java,
        )

        val lifecycle = ZLinkFrameworkLifecycle(
            options,
            FakeZLinkBackendAdapterFactory(),
            ZLinkHandlerActivator.reflection(),
        )
        val failure = assertThrows<ZLinkConfigurationException> {
            lifecycle.start()
        }

        assertTrue(failure.message!!.contains("duplicate client/server request handler packet name"))
    }

    @Test
    fun lifecycleAcceptsKotlinSuspendingSpotActorInterfaceHandlers() {
        val options = DefaultZLinkFrameworkOptions()
        options.addHandlersFromPackageOf(KotlinSuspendHandlerMarker::class.java)
        val node = systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addSpotMesh(options, "rooms")
        node.enableRouter("inproc://rooms")
        node.objects().server().addSpotFactory("InterfaceSpot", InterfaceSpot::class.java) { factory -> factory.disableRelocation() }
        node.objects().server().addActorFactory("player", PlayerActor::class.java, PlayerActorFactory::class.java) { factory -> factory.recreateOnRelocation() }

        val lifecycle = ZLinkFrameworkLifecycle(
            options,
            FakeZLinkBackendAdapterFactory(),
            ZLinkHandlerActivator.reflection(),
        )
        try {
            lifecycle.start()
        } finally {
            lifecycle.stop()
        }
    }

    @Test
    fun springLifecycleDiscoversKotlinSuspendAnnotationBeanType() {
        AnnotationConfigApplicationContext().use { context ->
            val backendFactory = FakeZLinkBackendAdapterFactory()
            context.registerBean(
                ZLinkBackendAdapterProvider::class.java,
                java.util.function.Supplier { backendFactory },
            )
            context.register(
                SpringSuspendFrameworkConfig::class.java,
                ZLinkFrameworkAutoConfiguration::class.java,
            )
            context.refresh()

            assertTrue(
                backendFactory.calls().contains("router.bind.tcp://127.0.0.1:40502"),
                backendFactory.calls().toString(),
            )
        }
    }

    private fun requestContext(channelName: String) =
        object : ZLinkMessageContext {
            override fun meshName() = java.util.Optional.empty<String>()
            override fun channelName() = java.util.Optional.of(channelName)
            override fun packetName() = "ProfileRequest"
            override fun contentType() = java.util.Optional.empty<String>()
            override fun metadata() = emptyMap<String, String>()
            override fun correlationId() = java.util.Optional.empty<String>()
        }
}

class KotlinSuspendHandlerMarker

@ZLinkHandlerGroup("kotlin-interface-channel")
class KotlinSuspendingInterfaceRequestHandler :
    ZLinkSuspendingRequestHandler<ProfileRequest, ProfileReply> {
    override suspend fun handle(
        request: ProfileRequest,
        context: ZLinkMessageContext,
    ): ProfileReply =
        ProfileReply("${context.channelName().orElse("missing")}:${request.name}")
}

@ZLinkHandlerGroup("kotlin-interface-spot")
@systems.zlink.framework.handlers.ZLinkSpotTimer(name = "interface-timer", periodMillis = 1000)
class KotlinSuspendingInterfaceTimerHandler :
    ZLinkSuspendingSpotTimerHandler<InterfaceSpot> {
    override suspend fun handle(spot: InterfaceSpot, tick: systems.zlink.framework.spots.ZLinkTimerTick) {
    }
}

class KotlinCoroutineContextHandler {
    @ZLinkRequest
    suspend fun request(request: ProfileRequest): ProfileReply =
        ProfileReply(if (coroutineContext[Job] != null) "coroutine:${request.name}" else "missing")
}

class KotlinYieldAfterDispatcherSwitchHandler(
    private val dispatcher: CoroutineDispatcher,
    private val replyStage: CompletableFuture<ProfileReply>,
) {
    @ZLinkRequest
    suspend fun request(request: ProfileRequest): ProfileReply {
        delay(1)
        return withContext(dispatcher) {
            replyStage.await()
        }
    }
}

class KotlinDispatcherObservationHandler {
    val threadName: AtomicReference<String> = AtomicReference()
    val hasCoroutineJob: AtomicReference<Boolean> = AtomicReference(false)

    @ZLinkRequest
    suspend fun request(request: ProfileRequest): ProfileReply {
        threadName.set(Thread.currentThread().name)
        hasCoroutineJob.set(coroutineContext[Job] != null)
        return ProfileReply(request.name)
    }
}

@ZLinkHandlerGroup("kotlin-channel")
class KotlinSpringSuspendChannelHandler(private val dependency: SuspendDependency) {
    @ZLinkRequest
    suspend fun request(request: ProfileRequest, context: ZLinkMessageContext): ProfileReply =
        ProfileReply("${context.channelName().orElse("missing")}:${request.name}:${dependency.value}")

    @ZLinkSend
    suspend fun send(message: ProfileGreeting, context: ZLinkMessageContext) {
        ObservedValues.lastSend.set("${context.packetName()}:${message.value}")
    }
}

@ZLinkHandlerGroup("kotlin-fanout")
class KotlinSpringSuspendPublishHandler {
    @ZLinkPublish
    suspend fun publish(message: ProfileEvent, context: ZLinkPublishMessageContext) {
        ObservedValues.lastPublish.set("${context.topic()}:${message.value}")
    }
}

@ZLinkHandlerGroup("kotlin-spot")
class KotlinSpringSuspendSpotActorHandler {
    @ZLinkSpotActorRequest
    suspend fun request(actor: PlayerActor, request: PlayerCommand): PlayerReply =
        PlayerReply("${actor.context().actorId()}:${request.value}")

    @ZLinkSpotActorSend
    suspend fun send(actor: PlayerActor, message: PlayerEvent) {
        ObservedValues.lastActorSend.set("${actor.context().actorId()}:${message.value}")
    }

}

@ZLinkHandlerGroup("kotlin-failure")
class KotlinSuspendFailureHandler {
    @ZLinkRequest(packetName = "FailProfile")
    suspend fun fail(request: ProfileRequest): ProfileReply {
        throw IllegalStateException("boom:${request.name}")
    }

    @ZLinkRequest(packetName = "CancelProfile")
    suspend fun cancel(request: ProfileRequest): ProfileReply {
        throw CancellationException("cancel:${request.name}")
    }
}

class JavaProfileRequestHandler : systems.zlink.framework.channels.ZLinkRequestHandler<ProfileRequest, ProfileReply> {
    override fun handle(
        request: ProfileRequest,
        context: ZLinkMessageContext,
    ) = CompletableFuture.completedFuture(ProfileReply(request.name))
}

class PlayerActorFactory : ZLinkActorFactory {
    override fun create(context: ZLinkActorContext) =
        CompletableFuture.completedFuture<ZLinkActor>(
            PlayerActor(context.actorId()),
        )
}

@ZLinkHandlerGroup("kotlin-interface-spot")
class KotlinSuspendingSpotActorInterfaceHandler :
    ZLinkSuspendingSpotActorRequestHandler<InterfaceSpot, PlayerActor, InterfacePlayerCommand, InterfacePlayerReply> {
    override suspend fun handle(
        spot: InterfaceSpot,
        actor: PlayerActor,
        context: ZLinkMessageContext,
        request: InterfacePlayerCommand,
    ): InterfacePlayerReply =
        InterfacePlayerReply("${actor.context().actorId()}:${request.value}")
}

@ZLinkHandlerGroup("kotlin-interface-spot")
class SecondKotlinSuspendingSpotActorInterfaceHandler :
    ZLinkSuspendingSpotActorRequestHandler<
        InterfaceSpot,
        PlayerActor,
        SecondInterfacePlayerCommand,
        SecondInterfacePlayerReply,
        > {
    override suspend fun handle(
        spot: InterfaceSpot,
        actor: PlayerActor,
        context: ZLinkMessageContext,
        request: SecondInterfacePlayerCommand,
    ): SecondInterfacePlayerReply =
        SecondInterfacePlayerReply("${actor.context().actorId()}:${request.value}")
}

@Configuration(proxyBeanMethods = false)
open class SpringSuspendHandlerConfig {
    @Bean
    open fun dependency() = SuspendDependency("injected")

    @Bean
    open fun handler(dependency: SuspendDependency) = KotlinSpringSuspendChannelHandler(dependency)
}

@Configuration(proxyBeanMethods = false)
@EnableZLinkFramework
open class SpringSuspendFrameworkConfig {
    @Bean
    open fun dependency() = SuspendDependency("injected")

    @Bean
    open fun handler(dependency: SuspendDependency) = KotlinSpringSuspendChannelHandler(dependency)

    @Bean
    open fun frameworkConfigurer() = ZLinkFrameworkConfigurer { options ->
        options.setDefaultRequestTimeout(Duration.ofSeconds(1))
        options.addHandlersFromPackageOf(KotlinSuspendHandlerMarker::class.java)
        val channel = options.addClientServerChannel("profile").server()
            .setBindHost("127.0.0.1")
            .listen(40502)
        channel.addHandlerGroup("kotlin-channel")
    }
}

data class SuspendDependency(val value: String)

object ObservedValues {
    val lastSend: AtomicReference<String> = AtomicReference()
    val lastPublish: AtomicReference<String> = AtomicReference()
    val lastActorSend: AtomicReference<String> = AtomicReference()
    val lastActorJoined: AtomicReference<String> = AtomicReference()
    val lastActorLeft: AtomicReference<String> = AtomicReference()
}

@ZLinkPacket("ProfileRequest")
data class ProfileRequest(val name: String)

data class ProfileReply(val value: String)

class CompletionStageHandler {
    fun handle(request: ProfileRequest) =
        CompletableFuture.completedFuture(ProfileReply("stage:${request.name}"))
}

@ZLinkPacket("ProfileGreeting")
data class ProfileGreeting(val value: String)

@ZLinkPacket("ProfileEvent")
data class ProfileEvent(val value: String)

class PlayerActor(private val id: String) : ZLinkActor {
    override fun context(): ZLinkActorContext = object : ZLinkActorContext {
        override fun actorId(): String = id
        override fun objectGeneration(): Long = 1L
        override fun meshName(): String = "test"
        override fun spotId() = java.util.Optional.empty<String>()
        override fun boundSession() = null
        override fun joinSpot(spotId: String) = unsupportedJoinCall()
        override fun joinSpot(spotId: String, request: Any) =
            unsupportedJoinCall()
        override fun joinEntrySpot() = unsupportedJoinCall()
        override fun joinEntrySpot(request: Any) = unsupportedJoinCall()
    }

    private fun unsupportedJoinCall(): systems.zlink.framework.actors.ZLinkActorJoinCall =
        throw UnsupportedOperationException("test actor does not join spots")
}

data class JoinRequest(val value: String)

data class JoinReply(val value: String)

data class PlayerCommand(val value: String)

data class PlayerReply(val value: String)

data class PlayerEvent(val value: String)

@ZLinkPacket("InterfacePlayerCommand")
data class InterfacePlayerCommand(val value: String)

data class InterfacePlayerReply(val value: String)

@ZLinkPacket("SecondInterfacePlayerCommand")
data class SecondInterfacePlayerCommand(val value: String)

data class SecondInterfacePlayerReply(val value: String)

class InterfaceSpot(private val spotContext: ZLinkSpotContext) : ZLinkSpot<PlayerActor> {
    override fun context(): ZLinkSpotContext = spotContext

    override fun onJoinedActor(actor: PlayerActor) = CompletableFuture.completedFuture<Void>(null)

    override fun onLeaveActor(actor: PlayerActor) = CompletableFuture.completedFuture<Void>(null)
}
