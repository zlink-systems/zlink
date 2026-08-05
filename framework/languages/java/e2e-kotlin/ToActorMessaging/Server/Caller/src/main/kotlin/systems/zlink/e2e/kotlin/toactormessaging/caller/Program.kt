package systems.zlink.e2e.kotlin.toactormessaging.caller

import java.time.Duration
import kotlinx.coroutines.runBlocking
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.context.annotation.Bean
import systems.zlink.contracts.core.RoutingId
import systems.zlink.e2e.kotlin.toactormessaging.shared.Contracts
import systems.zlink.e2e.kotlin.toactormessaging.shared.Env
import systems.zlink.e2e.kotlin.toactormessaging.shared.JsonHttp
import systems.zlink.framework.actors.ActorRef
import systems.zlink.framework.actors.ZLinkActorClient
import systems.zlink.framework.actors.ZLinkActorDirectory
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind
import systems.zlink.framework.errors.ZLinkFrameworkException
import systems.zlink.framework.kotlin.awaitReply
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer

fun main(args: Array<String>) {
    Env.configure(args)
    boot("main builder")
    val builder = SpringApplicationBuilder(Program::class.java)
        .web(WebApplicationType.NONE)
    boot("main keepAlive")
    builder.application().setKeepAlive(true)
    boot("main run")
    builder.run()
    boot("main run done")
}

@EnableZLinkFramework
@SpringBootApplication(proxyBeanMethods = false)
class Program {
    @Bean(destroyMethod = "close")
    fun http(
        actors: ZLinkActorClient,
        directory: ZLinkActorDirectory,
    ): JsonHttp {
        boot("http create")
        val http = JsonHttp(Env.get("callerHttpEndpoint"))
        boot("http route health")
        http.get("/health") { mapOf("status" to "ok") }
        boot("http route send")
        http.post("/send", Contracts.ActorCallRequest::class.java) { request ->
            runBlocking {
                try {
                    val actorRef = findActorOrThrow(directory, request.actorId())
                    actors.sendToActor(
                        actorRef,
                        Contracts.ActorNotify(request.scenario(), request.actorId(), request.value()),
                    )
                        .submit()
                    Contracts.ActorCallResponse.ok(request.scenario(), request.actorId(), "sent")
                } catch (ex: RuntimeException) {
                    failed(request, ex)
                }
            }
        }
        boot("http route request")
        http.post("/request", Contracts.ActorCallRequest::class.java) { request ->
            runBlocking {
                try {
                    val actorRef = findActorOrThrow(directory, request.actorId())
                    val reply = actors.requestToActor(
                        actorRef,
                        Contracts.ActorAsk(request.scenario(), request.actorId(), request.value()),
                    )
                        .timeout(Duration.ofSeconds(5))
                        .awaitReply(Contracts.ActorReply::class.java)
                    Contracts.ActorCallResponse.ok(request.scenario(), request.actorId(), reply.value())
                } catch (ex: RuntimeException) {
                    failed(request, ex)
                }
            }
        }
        boot("http start")
        http.start()
        boot("http start done")
        return http
    }

    @Bean
    fun framework(): ZLinkFrameworkConfigurer {
        boot("framework configurer bean")
        return ZLinkFrameworkConfigurer { options ->
            boot("configureDispatch")
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(Env.get("logDirectory", "logs") + "/caller-flow.log")
                .traceLabel("kotlin-to-actor-caller")
            boot("configureDispatch done")
            boot("addLocationStore")
            options.addLocationStore(
                ZLinkRedisLocationStore(
                    ZLinkRedisLocationOptions()
                        .setConnectionString(Env.get("redisLocationEndpoint"))
                        .setKeyPrefix(Env.get("locationKeyPrefix")),
                ),
            )
            boot("addLocationStore done")
            boot("addRouteMesh")
            val spotMesh = options.addRouteMesh(Contracts.SPOT_MESH)
            boot("addRouteMesh done")
            boot("listen")
            spotMesh.listen(Env.get("callerSpotEndpoint"))
            boot("listen done")
            boot("setRoutingId")
            spotMesh.setRoutingId(RoutingId.from(Env.get("callerRid", "caller")))
            boot("setRoutingId done")
        }
    }
}

private fun boot(step: String) {
    println("[boot] role=caller step=$step")
}

private fun findActorOrThrow(
    directory: ZLinkActorDirectory,
    actorId: String,
): ActorRef {
    return directory.find(actorId).toCompletableFuture().join().orElseThrow {
        ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.NOT_FOUND,
            "Actor route '$actorId' was not found.",
        )
    }
}

private fun failed(
    request: Contracts.ActorCallRequest,
    ex: RuntimeException,
): Contracts.ActorCallResponse {
    var current: Throwable = ex
    while (current.cause != null) {
        if (current is ZLinkFrameworkException) {
            return Contracts.ActorCallResponse.failed(
                request.scenario(),
                request.actorId(),
                current.kind().name,
            )
        }
        current = current.cause!!
    }
    if (current is ZLinkFrameworkException) {
        return Contracts.ActorCallResponse.failed(
            request.scenario(),
            request.actorId(),
            current.kind().name,
        )
    }
    return Contracts.ActorCallResponse.failed(request.scenario(), request.actorId(), current.javaClass.simpleName)
}
