package systems.zlink.e2e.kotlin.spotservice.client.support

import systems.zlink.framework.kotlin.*

import java.net.URI
import java.time.Duration
import systems.zlink.stream.connector.ZLinkStreamCompression
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions
import systems.zlink.stream.connector.ZLinkStreamDispatchMode

internal val REQUEST_TIMEOUT: Duration = Duration.ofSeconds(5)

internal fun createStreamConnector(endpoint: String): ZLinkKotlinStreamConnector =
    createStreamConnector(endpoint, ZLinkStreamDispatchMode.IMMEDIATE, true)

internal fun createStreamConnector(
    endpoint: String,
    dispatchMode: ZLinkStreamDispatchMode,
    skipServerCertificateValidation: Boolean,
): ZLinkKotlinStreamConnector =
    ZLinkStreamConnectorFactory.create(
        ZLinkStreamConnectorOptions(
            URI.create(endpoint),
            dispatchMode,
            REQUEST_TIMEOUT,
            2,
            Duration.ofSeconds(5),
            64 * 1024,
            64 * 1024,
            true,
            Duration.ofSeconds(1),
            Duration.ofSeconds(5),
            false,
            Duration.ofMillis(250),
            Duration.ofSeconds(5),
            2.0,
            skipServerCertificateValidation,
            ZLinkStreamCompression.LZ4,
            null,
            null,
        ),
    ).kotlin()
