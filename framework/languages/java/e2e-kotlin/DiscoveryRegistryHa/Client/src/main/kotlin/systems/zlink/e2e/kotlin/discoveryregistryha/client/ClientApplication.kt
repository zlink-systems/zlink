package systems.zlink.e2e.kotlin.discoveryregistryha.client

import com.fasterxml.jackson.databind.ObjectMapper
import org.springframework.boot.ApplicationArguments
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.context.annotation.Bean
import systems.zlink.e2e.kotlin.discoveryregistryha.ClientOptions
import systems.zlink.e2e.kotlin.discoveryregistryha.ClientScenario

@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackages = ["systems.zlink.e2e.kotlin.discoveryregistryha.client"],
)
class ClientApplication {
    companion object {
        @JvmStatic
        fun run(vararg args: String) {
            val context = SpringApplicationBuilder(ClientApplication::class.java)
                .web(WebApplicationType.NONE)
                .run(*args)
            try {
                context.getBean(ClientScenario::class.java).run()
                println("discovery-registry-ha kotlin e2e result=passed")
            } finally {
                context.close()
            }
        }
    }

    @Bean
    fun objectMapper(): ObjectMapper = ObjectMapper()

    @Bean
    fun clientOptions(args: ApplicationArguments): ClientOptions =
        ClientOptions.parse(args.sourceArgs)

    @Bean
    fun clientScenario(
        json: ObjectMapper,
        clientOptions: ClientOptions,
    ): ClientScenario =
        ClientScenario(json, clientOptions)
}
