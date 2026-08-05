package systems.zlink.framework.kotlin

import java.time.Instant
import java.util.Optional
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertFalse
import org.junit.jupiter.api.Test
import systems.zlink.framework.monitoring.ZLinkMeshNodeSnapshot
import systems.zlink.framework.monitoring.ZLinkPlacementSnapshot
import systems.zlink.framework.monitoring.ZLinkTopologyReason
import systems.zlink.framework.monitoring.ZLinkTopologyState

class KotlinCapacityMonitoringContractTest {
    @Test
    fun `Java placement status keeps the exact Kotlin-visible shape`() {
        val placement = ZLinkPlacementSnapshot(
            false,
            3,
            2,
            Optional.of(ZLinkTopologyReason.CAPACITY_EXCEEDED),
        )
        val snapshot = ZLinkMeshNodeSnapshot(
            "mesh",
            ZLinkTopologyState.READY,
            true,
            0,
            emptyList(),
            emptyList(),
            placement,
            1,
            Instant.now(),
        )

        assertFalse(snapshot.placement().isAvailable)
        assertEquals(3, snapshot.placement().activeActorCount())
        assertEquals(2, snapshot.placement().activeSpotCount())
        assertEquals(
            ZLinkTopologyReason.CAPACITY_EXCEEDED,
            snapshot.placement().unavailableReason().orElseThrow(),
        )
    }
}
