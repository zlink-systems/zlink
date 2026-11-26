package MyGame.table

import MyGame.*
import java.util.concurrent.TimeoutException
import org.slf4j.LoggerFactory
import systems.zlink.framework.errors.ZLinkFrameworkException
import systems.zlink.framework.errors.ZLinkOperationCanceledException
import systems.zlink.framework.kotlin.*
import systems.zlink.framework.spots.*

class RoomProvisioner(
    override val context: ZLinkInstanceSpotContext,
    private val spots: ZLinkSpotManager,
) : ZLinkSuspendingInstanceSpot() {
    override fun configure() {
        context.handlers().addPacket(ProvisionRoom::class.java)
    }

    internal suspend fun prepare(message: PrepareRoom) {
        val id = message.assignmentId
        try {
            val created = spots.kotlin().getOrCreate(id, Names.TABLE).inMesh(Names.MESH).await()
            if (created.state() != ZLinkSpotCreateState.REJECTED) {
                context.outbound().kotlin().sendToSpot(id, message).await()
                return
            }
        } catch (failure: ZLinkOperationCanceledException) {
            throw failure
        } catch (failure: ZLinkFrameworkException) {
            logger.warn("Room preparation failed for {}", id, failure)
        } catch (failure: TimeoutException) {
            logger.warn("Room preparation timed out for {}", id, failure)
        }
        context
            .outbound()
            .kotlin()
            .sendToSpot(Names.POOL_KEY, RoomPrepared(id, false))
            .instanceSpot(Names.POOL)
            .inMesh(Names.MESH)
            .await()
    }

    private companion object {
        val logger = LoggerFactory.getLogger(RoomProvisioner::class.java)
    }
}

class ProvisionRoom : ZLinkSuspendingSpotPacketHandler<RoomProvisioner, PrepareRoom> {
    override suspend fun handle(spot: RoomProvisioner, message: PrepareRoom) = spot.prepare(message)
}
