package systems.zlink.e2e.kotlin.registrymessaging.consumer

import java.io.File
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.context.annotation.Bean
import systems.zlink.e2e.kotlin.registrymessaging.consumer.Configuration.ConsumerOptions
import systems.zlink.e2e.kotlin.registrymessaging.consumer.Endpoints.ConsumerEndpoints
import systems.zlink.e2e.kotlin.registrymessaging.shared.Contracts
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer

private lateinit var parsedOptions: ConsumerOptions

fun main(args: Array<String>) {
    parsedOptions = ConsumerOptions.parse(args)
    File(parsedOptions.logDir).mkdirs()
    val context = SpringApplicationBuilder(ConsumerApplication::class.java)
        .web(WebApplicationType.NONE)
        .run(*args)
    ConsumerEndpoints(parsedOptions, context).start()
}

@EnableZLinkFramework
@SpringBootApplication(proxyBeanMethods = false)
class ConsumerApplication {
    @Bean
    fun consumerOptions(): ConsumerOptions = parsedOptions

    @Bean
    fun framework(options: ConsumerOptions): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { framework ->
            framework.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile("${options.logDir}/${options.traceLabel}-flow.log")
                .traceLabel(options.traceLabel)

            val profile = framework.addClientServerChannel(Contracts.PROFILE_CHANNEL)
            val client = profile.client()
            if (options.providerEndpoints.isEmpty()) {
                framework.addClientServerChannel(Contracts.WORKFLOW_CHANNEL).client()
            } else {
                for (endpoint in options.providerEndpoints) {
                    client.connect(endpoint)
                }
            }

        }

    @Bean
    fun locationStore(options: ConsumerOptions): ZLinkRedisLocationStore =
        ZLinkRedisLocationStore(
            ZLinkRedisLocationOptions()
                .setConnectionString(options.redisLocationEndpoint)
                .setKeyPrefix(options.locationKeyPrefix),
        )
}
