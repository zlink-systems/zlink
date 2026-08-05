package systems.zlink.e2e.kotlin.runtimemonitoring.client

import systems.zlink.e2e.kotlin.runtimemonitoring.Env

class ClientOptions(
    val apiEndpoint: String = Env.get("e2e.api.endpoint"),
    val handshakeEndpoint: String = Env.get("e2e.handshake.endpoint"),
    val filteredApiEndpoint: String = Env.get("e2e.filtered.api.endpoint"),
    val throwingApiEndpoint: String = Env.get("e2e.throwing.api.endpoint"),
    val serviceHttp: String = Env.get("e2e.service.http"),
    val filteredServiceHttp: String = Env.get("e2e.filtered.service.http"),
    val throwingServiceHttp: String = Env.get("e2e.throwing.service.http"),
    val triggerHttp: String = Env.get("e2e.trigger.http"),
    val logDir: String = Env.get("e2e.log.dir"),
    val filteredServiceBin: String = Env.get("e2e.filtered.service.bin"),
    val scenario: String = Env.get("e2e.scenario", "all"),
)
