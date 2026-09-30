package systems.zlink.framework.perf.servers.actorcaller

import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withPermit
import kotlinx.coroutines.future.await
import systems.zlink.framework.actors.ZLinkActorCreateResult
import systems.zlink.framework.actors.ZLinkActorManager
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime
import systems.zlink.framework.perf.Evidence
import systems.zlink.framework.perf.Measurement
import systems.zlink.framework.perf.PerfActorType
import systems.zlink.framework.perf.Polling
import systems.zlink.framework.perf.RoleConfig

class ActorCallerSetup(
    private val measurement: Measurement,
    private val actors: ZLinkActorManager,
    private val mesh: ZLinkRouteMeshRuntime,
) {
    private val config: RoleConfig = measurement.config()

    suspend fun createActors(): Map<String, Any> {
        Polling.until({ mesh.snapshot(config.meshName()).readyPeerCount() > 0 }, 10,
            config.workload().setupTimeoutMs().toLong()).await()
        val semaphore = Semaphore(config.workload().connectConcurrency())
        val results = coroutineScope {
            config.actorIds().map { actorId ->
                async {
                    semaphore.withPermit {
                        actors.kotlin().getOrCreate(actorId, PerfActorType.NAME)
                            .inMesh(config.meshName())
                            .timeout(java.time.Duration.ofMillis(config.workload().setupTimeoutMs().toLong()))
                            .await()
                    }
                }
            }.awaitAll()
        }
        val created = results.count { it is ZLinkActorCreateResult.Created }
        val existing = results.count { it is ZLinkActorCreateResult.Existing }
        check(created + existing == results.size) { "Actor creation was rejected." }
        return Evidence.of("actorCreate", "Kotlin ZLinkActorManager.getOrCreate(...).await()",
            mapOf("created" to created, "existing" to existing, "expectedActors" to results.size))
    }
}
