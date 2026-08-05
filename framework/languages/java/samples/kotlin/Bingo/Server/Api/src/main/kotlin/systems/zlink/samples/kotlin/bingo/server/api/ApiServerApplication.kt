package systems.zlink.samples.kotlin.bingo.server.api

import kotlinx.coroutines.Dispatchers
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.boot.context.properties.EnableConfigurationProperties
import org.springframework.context.annotation.Bean
import org.springframework.core.env.StandardEnvironment
import java.nio.file.Path
import systems.zlink.framework.codecs.protobuf.ZLinkProtobufCodec
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.kotlin.configureDispatch
import systems.zlink.framework.kotlin.useCoroutineHandlers
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleLocationStore
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleNames
import systems.zlink.samples.kotlin.bingo.server.configuration.SampleTopology



@EnableZLinkFramework
@EnableConfigurationProperties(SampleTopology::class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackageClasses = [ApiServerApplication::class],
)
class ApiServerApplication {
    @Bean
    fun apiFramework(topology: SampleTopology): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { options ->
            options.addHandlersFromPackageOf(ApiServerApplication::class.java)
            options.useCoroutineHandlers(Dispatchers.Default)
            options.configureDispatch {
                messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                traceLogFile(topology.logDirectory + "/flow-api.log")
                traceLabel("api")
            }
            options.codecs().use(ZLinkProtobufCodec.defaultCodec())
            options.configureLocations()
            val api = options.addRouteMesh(SampleNames.Mesh)
                .setRoutingIdPrefix("api")
                .listen(topology.selectedApiMeshEndpoint())
            api.objects().client()
            val apiChannelEndpoint = java.net.URI.create(topology.selectedApiChannelEndpoint())
            options.addClientServerChannel(SampleNames.ApiChannel)
                .server()
                .setBindHost(apiChannelEndpoint.host)
                .listen(apiChannelEndpoint.port)
                .addHandlerGroup(SampleNames.ApiChannel)
            options.addRouteMesh(SampleNames.MatchmakingMesh)
                .setRoutingIdPrefix("api-matchmaking")
                .listen(topology.apiMatchmakingRouterEndpoint)
                .objects().client()
        }

    @Bean
    fun locationStore(topology: SampleTopology): ZLinkRedisLocationStore = SampleLocationStore.create(topology)

    companion object {
        fun run(args: Array<String> = emptyArray()): AutoCloseable {
            val environment = StandardEnvironment().apply {
                propertySources.remove(StandardEnvironment.SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME)
                propertySources.remove(StandardEnvironment.SYSTEM_PROPERTIES_PROPERTY_SOURCE_NAME)
            }
            val configPath = SampleTopology.configPath(args)
            val builder = SpringApplicationBuilder(ApiServerApplication::class.java)
                .environment(environment)
                .properties("spring.config.location=${Path.of(configPath).toAbsolutePath().toUri()}")
                .web(WebApplicationType.NONE)
            builder.application().setKeepAlive(true)
            val context = builder.run()
            return AutoCloseable { context.close() }
        }
    }
}
