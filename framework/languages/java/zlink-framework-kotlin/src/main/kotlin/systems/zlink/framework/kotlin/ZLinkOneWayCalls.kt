package systems.zlink.framework.kotlin

import java.time.Duration
import java.util.concurrent.CompletionStage
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.reflect.KClass
import systems.zlink.contracts.core.RoutingId
import systems.zlink.framework.actors.ZLinkActorClient
import systems.zlink.framework.actors.ZLinkActorCreateCall
import systems.zlink.framework.actors.ZLinkActorCreateResult
import systems.zlink.framework.actors.ZLinkActorGetOrCreateCall
import systems.zlink.framework.actors.ZLinkActorManager
import systems.zlink.framework.actors.ZLinkActorRequestCall
import systems.zlink.framework.actors.ZLinkActorSendCall
import systems.zlink.framework.actors.ZLinkBoundSession
import systems.zlink.framework.actors.ZLinkBoundSessionSendCall
import systems.zlink.framework.channels.ZLinkClient
import systems.zlink.framework.channels.ZLinkFanoutClient
import systems.zlink.framework.channels.ZLinkFanoutPublishCall
import systems.zlink.framework.channels.ZLinkRequestCall
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.channels.ZLinkSendCall
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind
import systems.zlink.framework.errors.ZLinkFrameworkException
import systems.zlink.framework.spots.ZLinkWorkerCall
import systems.zlink.framework.spots.ZLinkSpotCreateCall
import systems.zlink.framework.spots.ZLinkSpotCreateResult
import systems.zlink.framework.spots.ZLinkSpotGetOrCreateCall
import systems.zlink.framework.spots.ZLinkSpotManager
import systems.zlink.framework.spots.ZLinkSpotRequestCall
import systems.zlink.framework.spots.ZLinkSpotSendCall
import systems.zlink.framework.streams.ZLinkSessionActor
import systems.zlink.framework.streams.ZLinkSessionClient

private class JavaMessageSendCall(
    private val call: ZLinkSendCall,
    private val terminal: KotlinSingleUse = KotlinSingleUse(),
) : ZLinkKotlinMessageSendCall {
    override fun metadata(key: String, value: String): ZLinkKotlinMessageSendCall =
        JavaMessageSendCall(call.metadata(key, value), terminal)

    override suspend fun await() {
        terminal.enter()
        awaitFrameworkStage(call.submit())
    }
}

private class JavaActorMessageSendCall(
    private val call: ZLinkActorSendCall,
    private val terminal: KotlinSingleUse = KotlinSingleUse(),
) : ZLinkKotlinMessageSendCall {
    override fun metadata(key: String, value: String): ZLinkKotlinMessageSendCall =
        JavaActorMessageSendCall(call.metadata(key, value), terminal)

    override suspend fun await() {
        terminal.enter()
        awaitFrameworkStage(call.submit())
    }
}

private class JavaBoundSessionMessageSendCall(
    private val call: ZLinkBoundSessionSendCall,
    private val terminal: KotlinSingleUse = KotlinSingleUse(),
) : ZLinkKotlinMessageSendCall {
    override fun metadata(key: String, value: String): ZLinkKotlinMessageSendCall =
        JavaBoundSessionMessageSendCall(call.metadata(key, value), terminal)

    override suspend fun await() {
        terminal.enter()
        awaitFrameworkStage(call.submit())
    }
}

private class JavaSubmissionCall(
    private val call: ZLinkFanoutPublishCall,
    private val terminal: KotlinSingleUse = KotlinSingleUse(),
) : ZLinkKotlinSubmissionCall {
    override suspend fun await() {
        terminal.enter()
        awaitFrameworkStage(call.submit())
    }
}

private class DeferredSubmissionCall(
    private val submit: () -> CompletionStage<Void>,
    private val terminal: KotlinSingleUse = KotlinSingleUse(),
) : ZLinkKotlinSubmissionCall {
    override suspend fun await() {
        terminal.enter()
        awaitFrameworkStage(submit())
    }
}

private class KotlinSingleUse {
    private val consumed = AtomicBoolean()

    fun enter() {
        if (!consumed.compareAndSet(false, true)) {
            throw ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.INVALID_OPERATION,
                "call has already been submitted",
            )
        }
    }
}

private class JavaRequestCall<TReply : Any>(
    private val call: ZLinkRequestCall,
    private val replyType: Class<TReply>,
    private val terminal: KotlinSingleUse = KotlinSingleUse(),
) : ZLinkKotlinRequestCall<TReply> {
    override fun metadata(key: String, value: String): ZLinkKotlinRequestCall<TReply> =
        JavaRequestCall(call.metadata(key, value), replyType, terminal)

    override fun timeout(timeout: Duration): ZLinkKotlinRequestCall<TReply> =
        JavaRequestCall(call.timeout(timeout), replyType, terminal)

    override suspend fun await(): TReply {
        terminal.enter()
        return awaitFrameworkStage(call.submit(replyType))
    }

    override suspend fun yield(): TReply {
        terminal.enter()
        return awaitFrameworkStage(call.yield(replyType))
    }
}

private class JavaActorRequestCall<TReply : Any>(
    private val call: ZLinkActorRequestCall,
    private val replyType: Class<TReply>,
    private val terminal: KotlinSingleUse = KotlinSingleUse(),
) : ZLinkKotlinRequestCall<TReply> {
    override fun metadata(key: String, value: String): ZLinkKotlinRequestCall<TReply> =
        JavaActorRequestCall(call.metadata(key, value), replyType, terminal)

    override fun timeout(timeout: Duration): ZLinkKotlinRequestCall<TReply> =
        JavaActorRequestCall(call.timeout(timeout), replyType, terminal)

    override suspend fun await(): TReply {
        terminal.enter()
        return awaitFrameworkStage(call.submit(replyType))
    }

    override suspend fun yield(): TReply {
        terminal.enter()
        return awaitFrameworkStage(call.yield(replyType))
    }
}

private class JavaSpotSendCall(
    private val call: ZLinkSpotSendCall,
    private val terminal: KotlinSingleUse = KotlinSingleUse(),
) : ZLinkKotlinSpotSendCall {
    override fun metadata(key: String, value: String): ZLinkKotlinSpotSendCall =
        JavaSpotSendCall(call.metadata(key, value), terminal)

    override fun instanceSpot(): ZLinkKotlinSpotSendCall =
        JavaSpotSendCall(call.instanceSpot(), terminal)

    override fun instanceSpot(stableType: String): ZLinkKotlinSpotSendCall =
        JavaSpotSendCall(call.instanceSpot(stableType), terminal)

    override fun inMesh(meshName: String): ZLinkKotlinSpotSendCall =
        JavaSpotSendCall(call.inMesh(meshName), terminal)

    override suspend fun await() {
        terminal.enter()
        awaitFrameworkStage(call.submit())
    }
}

private class JavaSpotRequestCall<TReply : Any>(
    private val call: ZLinkSpotRequestCall,
    private val replyType: Class<TReply>,
    private val terminal: KotlinSingleUse = KotlinSingleUse(),
) : ZLinkKotlinSpotRequestCall<TReply> {
    override fun metadata(key: String, value: String): ZLinkKotlinSpotRequestCall<TReply> =
        JavaSpotRequestCall(call.metadata(key, value), replyType, terminal)

    override fun instanceSpot(): ZLinkKotlinSpotRequestCall<TReply> =
        JavaSpotRequestCall(call.instanceSpot(), replyType, terminal)

    override fun instanceSpot(stableType: String): ZLinkKotlinSpotRequestCall<TReply> =
        JavaSpotRequestCall(call.instanceSpot(stableType), replyType, terminal)

    override fun inMesh(meshName: String): ZLinkKotlinSpotRequestCall<TReply> =
        JavaSpotRequestCall(call.inMesh(meshName), replyType, terminal)

    override fun timeout(timeout: Duration): ZLinkKotlinSpotRequestCall<TReply> =
        JavaSpotRequestCall(call.timeout(timeout), replyType, terminal)

    override suspend fun await(): TReply {
        terminal.enter()
        return awaitFrameworkStage(call.submit(replyType))
    }

    override suspend fun yield(): TReply {
        terminal.enter()
        return awaitFrameworkStage(call.yield(replyType))
    }
}

private class JavaClient(
    private val client: ZLinkClient,
) : ZLinkKotlinClient {
    override fun sendToChannel(
        channelName: String,
        message: Any,
    ): ZLinkKotlinMessageSendCall =
        JavaMessageSendCall(client.sendToChannel(channelName, message))

    override fun <TReply : Any> requestToChannel(
        channelName: String,
        request: Any,
        replyType: KClass<TReply>,
    ): ZLinkKotlinRequestCall<TReply> =
        JavaRequestCall(client.requestToChannel(channelName, request), replyType.java)
}

private class JavaFanoutClient(
    private val client: ZLinkFanoutClient,
) : ZLinkKotlinFanoutClient {
    override fun publish(
        channelName: String,
        topic: String,
        event: Any,
    ): ZLinkKotlinSubmissionCall =
        JavaSubmissionCall(client.publish(channelName, topic, event))

    override fun publish(
        channelName: String,
        event: Any,
    ): ZLinkKotlinSubmissionCall =
        JavaSubmissionCall(client.publish(channelName, event))
}

private interface JavaSpotRouteCapability {
    fun sendToSpotCall(spotId: String, message: Any): ZLinkSpotSendCall

    fun requestToSpotCall(
        spotId: String,
        request: Any,
    ): ZLinkSpotRequestCall
}

private class JavaRouteClient(
    private val client: ZLinkRouteClient,
) : ZLinkKotlinRouteClient, JavaSpotRouteCapability {

    override fun sendToSpotCall(
        spotId: String,
        message: Any,
    ): ZLinkSpotSendCall = client.sendToSpot(spotId, message)

    override fun requestToSpotCall(
        spotId: String,
        request: Any,
    ): ZLinkSpotRequestCall = client.requestToSpot(spotId, request)

    override fun sendToNode(
        meshName: String,
        target: RoutingId,
        message: Any,
    ): ZLinkKotlinMessageSendCall =
        JavaMessageSendCall(client.sendToNode(meshName, target, message))

    override fun <TReply : Any> requestToNode(
        meshName: String,
        target: RoutingId,
        request: Any,
        replyType: KClass<TReply>,
    ): ZLinkKotlinRequestCall<TReply> =
        JavaRequestCall(client.requestToNode(meshName, target, request), replyType.java)

    override fun sendToChannel(
        channelName: String,
        message: Any,
    ): ZLinkKotlinMessageSendCall =
        JavaMessageSendCall(client.sendToChannel(channelName, message))

    override fun <TReply : Any> requestToChannel(
        channelName: String,
        request: Any,
        replyType: KClass<TReply>,
    ): ZLinkKotlinRequestCall<TReply> =
        JavaRequestCall(client.requestToChannel(channelName, request), replyType.java)
}

private class JavaActorClient(
    private val client: ZLinkActorClient,
) : ZLinkKotlinActorClient {
    override fun sendToActor(
        actorId: String,
        message: Any,
    ): ZLinkKotlinMessageSendCall =
        JavaActorMessageSendCall(client.sendToActor(actorId, message))

    override fun <TReply : Any> requestToActor(
        actorId: String,
        request: Any,
        replyType: KClass<TReply>,
    ): ZLinkKotlinRequestCall<TReply> {
        return JavaActorRequestCall(
            client.requestToActor(actorId, request),
            replyType.java,
        )
    }
}

private class JavaActorCreateCall(
    private var call: ZLinkActorCreateCall,
    private val terminal: KotlinSingleUse = KotlinSingleUse(),
) : ZLinkKotlinActorCreateCall {
    override fun inMesh(meshName: String): ZLinkKotlinActorCreateCall =
        apply { call = call.inMesh(meshName) }

    override fun request(request: Any): ZLinkKotlinActorCreateCall =
        apply { call = call.request(request) }

    override fun timeout(timeout: Duration): ZLinkKotlinActorCreateCall =
        apply { call = call.timeout(timeout) }

    override suspend fun await(): ZLinkActorCreateResult {
        terminal.enter()
        return awaitFrameworkStage(call.submit())
    }

    override suspend fun yield(): ZLinkActorCreateResult {
        terminal.enter()
        return awaitFrameworkStage(call.yield())
    }
}

private class JavaActorGetOrCreateCall(
    private var call: ZLinkActorGetOrCreateCall,
    private val terminal: KotlinSingleUse = KotlinSingleUse(),
) : ZLinkKotlinActorCreateCall {
    override fun inMesh(meshName: String): ZLinkKotlinActorCreateCall =
        apply { call = call.inMesh(meshName) }

    override fun request(request: Any): ZLinkKotlinActorCreateCall =
        apply { call = call.request(request) }

    override fun timeout(timeout: Duration): ZLinkKotlinActorCreateCall =
        apply { call = call.timeout(timeout) }

    override suspend fun await(): ZLinkActorCreateResult {
        terminal.enter()
        return awaitFrameworkStage(call.submit())
    }

    override suspend fun yield(): ZLinkActorCreateResult {
        terminal.enter()
        return awaitFrameworkStage(call.yield())
    }
}

private class JavaActorManager(
    private val manager: ZLinkActorManager,
) : ZLinkKotlinActorManager {
    override fun create(
        actorId: String,
        actorType: String,
    ): ZLinkKotlinActorCreateCall =
        JavaActorCreateCall(manager.create(actorId, actorType))

    override fun getOrCreate(
        actorId: String,
        actorType: String,
    ): ZLinkKotlinActorCreateCall =
        JavaActorGetOrCreateCall(manager.getOrCreate(actorId, actorType))

    override suspend fun destroy(actor: systems.zlink.framework.actors.ActorRef): Boolean =
        awaitFrameworkStage(manager.destroy(actor))
}

private class JavaSpotCreateCall(
    private var call: ZLinkSpotCreateCall,
    private val terminal: KotlinSingleUse = KotlinSingleUse(),
) : ZLinkKotlinSpotCreateCall {
    override fun inMesh(meshName: String): ZLinkKotlinSpotCreateCall =
        apply { call = call.inMesh(meshName) }

    override fun request(request: Any): ZLinkKotlinSpotCreateCall =
        apply { call = call.request(request) }

    override fun timeout(timeout: Duration): ZLinkKotlinSpotCreateCall =
        apply { call = call.timeout(timeout) }

    override suspend fun await(): ZLinkSpotCreateResult {
        terminal.enter()
        return awaitFrameworkStage(call.submit())
    }

    override suspend fun yield(): ZLinkSpotCreateResult {
        terminal.enter()
        return awaitFrameworkStage(call.yield())
    }
}

private class JavaSpotGetOrCreateCall(
    private var call: ZLinkSpotGetOrCreateCall,
    private val terminal: KotlinSingleUse = KotlinSingleUse(),
) : ZLinkKotlinSpotCreateCall {
    override fun inMesh(meshName: String): ZLinkKotlinSpotCreateCall =
        apply { call = call.inMesh(meshName) }

    override fun request(request: Any): ZLinkKotlinSpotCreateCall =
        apply { call = call.request(request) }

    override fun timeout(timeout: Duration): ZLinkKotlinSpotCreateCall =
        apply { call = call.timeout(timeout) }

    override suspend fun await(): ZLinkSpotCreateResult {
        terminal.enter()
        return awaitFrameworkStage(call.submit())
    }

    override suspend fun yield(): ZLinkSpotCreateResult {
        terminal.enter()
        return awaitFrameworkStage(call.yield())
    }
}

private class JavaSpotManager(
    private val manager: ZLinkSpotManager,
) : ZLinkKotlinSpotManager {
    override fun create(stableType: String): ZLinkKotlinSpotCreateCall =
        JavaSpotCreateCall(manager.create(stableType))

    override fun getOrCreate(
        spotId: String,
        stableType: String,
    ): ZLinkKotlinSpotCreateCall =
        JavaSpotGetOrCreateCall(manager.getOrCreate(spotId, stableType))
}

private class JavaSessionClient(
    private val client: ZLinkSessionClient,
) : ZLinkKotlinSessionClient {
    override fun send(message: Any): ZLinkKotlinSessionSendCall =
        JavaSessionSendCall(client.send(message))

    override fun reply(message: Any): ZLinkKotlinSessionReplyCall =
        JavaSessionReplyCall(client.reply(message))
}

private class JavaSessionSendCall(
    private var call: systems.zlink.framework.streams.ZLinkSessionSendCall,
    private val terminal: KotlinSingleUse = KotlinSingleUse(),
) : ZLinkKotlinSessionSendCall {
    override fun metadata(key: String, value: String): ZLinkKotlinSessionSendCall =
        apply { call = call.metadata(key, value) }

    override fun compress(): ZLinkKotlinSessionSendCall =
        apply { call = call.compress() }

    override fun timeout(timeout: Duration): ZLinkKotlinSessionSendCall =
        apply { call = call.timeout(timeout) }

    override suspend fun await() {
        terminal.enter()
        awaitFrameworkStage(call.submit())
    }
}

private class JavaSessionReplyCall(
    private var call: systems.zlink.framework.streams.ZLinkSessionReplyCall,
    private val terminal: KotlinSingleUse = KotlinSingleUse(),
) : ZLinkKotlinSessionReplyCall {
    override fun compress(): ZLinkKotlinSessionReplyCall =
        apply { call = call.compress() }

    override suspend fun await() {
        terminal.enter()
        awaitFrameworkStage(call.submit())
    }
}

fun ZLinkClient.kotlin(): ZLinkKotlinClient = JavaClient(this)

fun ZLinkFanoutClient.kotlin(): ZLinkKotlinFanoutClient = JavaFanoutClient(this)

fun ZLinkRouteClient.kotlin(): ZLinkKotlinRouteClient = JavaRouteClient(this)

fun ZLinkActorClient.kotlin(): ZLinkKotlinActorClient = JavaActorClient(this)

fun ZLinkActorManager.kotlin(): ZLinkKotlinActorManager = JavaActorManager(this)

fun ZLinkSpotManager.kotlin(): ZLinkKotlinSpotManager = JavaSpotManager(this)

fun ZLinkSessionClient.kotlin(): ZLinkKotlinSessionClient = JavaSessionClient(this)

fun ZLinkSessionActor.kotlin(): ZLinkKotlinSessionActor =
    object : ZLinkKotlinSessionActor {
        override fun relay(
            message: systems.zlink.framework.messaging.ZLinkMessage,
        ): ZLinkKotlinSubmissionCall =
            DeferredSubmissionCall({ this@kotlin.relay(message) })

        override fun relay(
            dispatch: systems.zlink.framework.streams.ZLinkSessionDispatchContext,
            message: systems.zlink.framework.messaging.ZLinkMessage,
        ): ZLinkKotlinSubmissionCall =
            DeferredSubmissionCall({ this@kotlin.relay(dispatch, message) })
    }

fun ZLinkBoundSession.kotlin(): ZLinkKotlinBoundSession =
    object : ZLinkKotlinBoundSession {
        override fun send(message: Any): ZLinkKotlinMessageSendCall =
            JavaBoundSessionMessageSendCall(this@kotlin.send(message))
    }

fun <T> ZLinkWorkerCall<T>.kotlin(): ZLinkKotlinWorkerCall<T> =
    object : ZLinkKotlinWorkerCall<T> {
        private val terminal = KotlinSingleUse()

        override suspend fun await(): T =
            terminal.run { enter(); awaitFrameworkStage(this@kotlin.submit()) }

        override suspend fun yield(): T =
            terminal.run { enter(); awaitFrameworkStage(this@kotlin.yield()) }
    }

inline fun <reified TReply : Any> ZLinkKotlinClient.requestToChannel(
    channelName: String,
    request: Any,
): ZLinkKotlinRequestCall<TReply> =
    requestToChannel(channelName, request, TReply::class)

inline fun <reified TReply : Any> ZLinkKotlinRouteClient.requestToNode(
    meshName: String,
    target: RoutingId,
    request: Any,
): ZLinkKotlinRequestCall<TReply> =
    requestToNode(meshName, target, request, TReply::class)

inline fun <reified TReply : Any> ZLinkKotlinRouteClient.requestToChannel(
    channelName: String,
    request: Any,
): ZLinkKotlinRequestCall<TReply> =
    requestToChannel(channelName, request, TReply::class)

inline fun <reified TReply : Any> ZLinkKotlinActorClient.requestToActor(
    actorId: String,
    request: Any,
): ZLinkKotlinRequestCall<TReply> =
    requestToActor(actorId, request, TReply::class)

fun ZLinkKotlinRouteClient.sendToSpot(
    spotId: String,
    message: Any,
): ZLinkKotlinSpotSendCall {
    val capability = this as? JavaSpotRouteCapability
        ?: throw IllegalStateException(
            "Spot routing extensions require a route client created by ZLinkRouteClient.kotlin()")
    return JavaSpotSendCall(capability.sendToSpotCall(spotId, message))
}

inline fun <reified TReply : Any> ZLinkKotlinRouteClient.requestToSpot(
    spotId: String,
    request: Any,
): ZLinkKotlinSpotRequestCall<TReply> =
    requestToSpot(spotId, request, TReply::class)

fun <TReply : Any> ZLinkKotlinRouteClient.requestToSpot(
    spotId: String,
    request: Any,
    replyType: KClass<TReply>,
): ZLinkKotlinSpotRequestCall<TReply> {
    val capability = this as? JavaSpotRouteCapability
        ?: throw IllegalStateException(
            "Spot routing extensions require a route client created by ZLinkRouteClient.kotlin()")
    return JavaSpotRequestCall(
        capability.requestToSpotCall(spotId, request),
        replyType.java,
    )
}
