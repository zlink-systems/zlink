package systems.zlink.samples.kotlin.deliverydispatch.server.configuration

import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.StandardOpenOption
import java.time.Instant
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.DeliveryStatus
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.DeliveryStatusChangedReq
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.DeliveryStatusNotify
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.ServerAssertionRes

class DeliveryEvidenceStore(stateDir: String) {
    private val root: Path = Path.of(stateDir)

    init {
        Files.createDirectories(root)
    }

    @Synchronized
    fun append(
        deliveryId: String,
        customerId: String,
        status: DeliveryStatus,
        courierId: String,
        occurredAt: Instant,
    ) {
        val line = listOf(deliveryId, customerId, status.name, courierId, occurredAt.toString()).joinToString("|")
        Files.writeString(
            evidenceFile(deliveryId),
            "$line\n",
            StandardOpenOption.CREATE,
            StandardOpenOption.APPEND,
        )
    }

    fun append(status: DeliveryStatusChangedReq) {
        append(
            deliveryId = status.deliveryId,
            customerId = "",
            status = status.status,
            courierId = status.courierId,
            occurredAt = status.occurredAt,
        )
    }

    fun read(deliveryId: String): List<String> {
        val file = evidenceFile(deliveryId)
        return if (Files.exists(file)) Files.readAllLines(file) else emptyList()
    }

    fun assertSequences(
        successfulDeliveryId: String,
        reassignedDeliveryId: String,
    ): ServerAssertionRes {
        val evidence = mutableListOf<String>()
        val successful = matches(
            actual = statuses(successfulDeliveryId),
            expected = listOf(
                ExpectedStatus(DeliveryStatus.Assigned, "courier-a"),
                ExpectedStatus(DeliveryStatus.Accepted, "courier-a"),
                ExpectedStatus(DeliveryStatus.PickedUp, "courier-a"),
                ExpectedStatus(DeliveryStatus.Delivered, "courier-a"),
            ),
            evidence = evidence,
            deliveryId = successfulDeliveryId,
        )
        val reassigned = matches(
            actual = statuses(reassignedDeliveryId),
            expected = listOf(
                ExpectedStatus(DeliveryStatus.Assigned, "courier-a"),
                ExpectedStatus(DeliveryStatus.Reassigned, "courier-b"),
                ExpectedStatus(DeliveryStatus.Accepted, "courier-b"),
                ExpectedStatus(DeliveryStatus.PickedUp, "courier-b"),
                ExpectedStatus(DeliveryStatus.Delivered, "courier-b"),
            ),
            evidence = evidence,
            deliveryId = reassignedDeliveryId,
        )
        return ServerAssertionRes(successful && reassigned, evidence.toTypedArray())
    }

    private fun evidenceFile(deliveryId: String): Path =
        root.resolve("$deliveryId.evidence")

    private fun statuses(deliveryId: String): List<DeliveryStatusNotify> =
        read(deliveryId).mapNotNull(::lineToStatus)

    private fun lineToStatus(line: String): DeliveryStatusNotify? {
        val parts = line.split("|")
        if (parts.size < 5) {
            return null
        }
        return DeliveryStatusNotify(
            deliveryId = parts[0],
            status = DeliveryStatus.valueOf(parts[2]),
            courierId = parts[3],
            occurredAt = Instant.parse(parts[4]),
        )
    }

    private fun matches(
        actual: List<DeliveryStatusNotify>,
        expected: List<ExpectedStatus>,
        evidence: MutableList<String>,
        deliveryId: String,
    ): Boolean {
        if (actual.size != expected.size) {
            evidence.add("$deliveryId expected ${expected.size} statuses but saw ${actual.size}")
            return false
        }
        actual.zip(expected).forEachIndexed { index, (actualStatus, expectedStatus) ->
            if (actualStatus.status != expectedStatus.status || actualStatus.courierId != expectedStatus.courierId) {
                evidence.add(
                    "$deliveryId status[$index] expected " +
                        "${expectedStatus.status}/${expectedStatus.courierId} but saw " +
                        "${actualStatus.status}/${actualStatus.courierId}",
                )
                return false
            }
        }
        evidence.add("$deliveryId status sequence verified")
        return true
    }

    private data class ExpectedStatus(
        val status: DeliveryStatus,
        val courierId: String,
    )
}
