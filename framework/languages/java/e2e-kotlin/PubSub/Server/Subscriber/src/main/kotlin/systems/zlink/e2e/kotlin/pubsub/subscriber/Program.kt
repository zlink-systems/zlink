package systems.zlink.e2e.kotlin.pubsub.subscriber

import com.fasterxml.jackson.databind.ObjectMapper
import java.util.concurrent.CompletableFuture
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.beans.factory.ObjectProvider
import org.springframework.context.annotation.Bean
import systems.zlink.e2e.kotlin.pubsub.shared.Contracts
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.configuration.ZLinkMessageFlowOutcome
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime

private lateinit var parsedOptions: SubscriberOptions

fun main(args: Array<String>) {
    parsedOptions = SubscriberOptions.parse(args)
    val builder = SpringApplicationBuilder(SubscriberApplication::class.java)
        .web(WebApplicationType.NONE)
    builder.application().setKeepAlive(true)
    builder.run(*args)
}

@EnableZLinkFramework
@SpringBootApplication(proxyBeanMethods = false)
class SubscriberApplication {
    @Bean
    fun subscriberOptions(): SubscriberOptions = parsedOptions

    @Bean
    fun scenarioState(options: SubscriberOptions): EvidenceStore =
        EvidenceStore(
            options.rid,
            options.topics,
            options.handlerDelayMillis,
        )

    @Bean
    fun objectMapper(): ObjectMapper = ObjectMapper()

    @Bean
    fun subscriberConnections(): SubscriberConnections = SubscriberConnections()

    @Bean
    fun fanoutObserverController(
        runtime: ObjectProvider<ZLinkFrameworkRuntime>,
    ): FanoutObserverController = FanoutObserverController(runtime)

    @Bean
    fun evidenceHttpServer(
        options: SubscriberOptions,
        state: EvidenceStore,
        json: ObjectMapper,
        connections: SubscriberConnections,
        observers: FanoutObserverController,
        runtime: ObjectProvider<ZLinkFrameworkRuntime>,
    ): OperationalEndpoints =
        OperationalEndpoints(state, json, options.httpEndpoint, connections, observers, runtime)

    @Bean
    fun subscriberFramework(
        options: SubscriberOptions,
        state: EvidenceStore,
        connections: SubscriberConnections,
    ): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { options ->
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile("${parsedOptions.logDir}/${state.subscriberRid}-flow.log")
                .traceLabel("kotlin-ps-${state.subscriberRid}")
                .setMessageFlowObserver { error ->
                    if (error.outcome() != ZLinkMessageFlowOutcome.ERROR) {
                        return@setMessageFlowObserver CompletableFuture.completedFuture(null)
                    }
                    state.record(
                        "DispatchError",
                        error.topic(),
                        "observer",
                        -1,
                        "${error.errorReason()}/${error.errorAction()}/${error.packetName()}",
                    )
                    CompletableFuture.completedFuture(null)
                }
            options.addHandlersFromPackageOf(EventMsgHandler::class.java)
            val channel = options.addFanoutChannel(Contracts.EVENT_CHANNEL)
            if (parsedOptions.mixedMode) {
                channel.enableSubscriber()
                    .subscriberConnections()
                    .connect(parsedOptions.manualEndpoint!!)
            } else if (parsedOptions.manualEndpoint != null) {
                channel.connect(parsedOptions.manualEndpoint)
            } else {
                channel.enableSubscriber()
            }
            connections.install(channel.subscriberConnections())
            channel.addHandlerGroup(Contracts.HANDLER_GROUP)
        }

    @Bean
    fun eventMsgHandler(state: EvidenceStore): EventMsgHandler =
        EventMsgHandler(state)

    @Bean
    fun locationStore(options: SubscriberOptions): ZLinkRedisLocationStore? =
        if (options.redisLocationEndpoint == null) {
            null
        } else {
            ZLinkRedisLocationStore(
                ZLinkRedisLocationOptions()
                    .setConnectionString(options.redisLocationEndpoint)
                    .setKeyPrefix(options.locationKeyPrefix!!),
            )
        }
}
