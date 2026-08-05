package systems.zlink.samples.kotlin.shoppingmall.server.configuration

import com.fasterxml.jackson.databind.JsonNode
import com.fasterxml.jackson.databind.ObjectMapper
import com.fasterxml.jackson.databind.node.ArrayNode
import com.fasterxml.jackson.databind.node.ObjectNode
import com.fasterxml.jackson.module.kotlin.registerKotlinModule
import java.nio.channels.FileChannel
import java.nio.charset.StandardCharsets
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.StandardOpenOption
import java.util.concurrent.TimeUnit
import java.util.concurrent.locks.LockSupport
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.CartSeed
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.OrderLineInput
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.OrderState
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.PaymentMethodSeed

/**
 * File-backed shared store for event stream, read-model projection, and
 * commerce business state. Every mutation takes an OS file lock, reads the
 * whole JSON document, applies the change, and writes it back; both
 * CommerceApi instances and both OrderWorkflow instances share one document.
 */
class CommerceStore(private val topology: SampleTopology) {
    val json: ObjectMapper = ObjectMapper().registerKotlinModule()
    private val stateFile: Path
    private val lockFile: Path
    private val stateMutex = Any()

    init {
        val dir = Path.of(topology.requiredStoreDirectory())
        Files.createDirectories(dir)
        stateFile = dir.resolve("commerce-state.json")
        lockFile = dir.resolve("commerce-state.lock")
    }

    data class StoredEvent(
        val eventId: String,
        val sourceCommandId: String?,
        val orderId: String,
        val eventType: String,
        val payload: JsonNode,
        val version: Long,
        val createdAtUnixMs: Long,
    )

    data class ReserveInventoryResult(val accepted: Boolean, val reservationId: String?, val reason: String?)

    data class AuthorizePaymentResult(val accepted: Boolean, val paymentId: String?, val reason: String?)

    data class IdempotencyMapping(
        val idempotencyKey: String,
        val orderId: String,
        val ownerInstanceId: String,
        val started: Boolean,
    )

    data class StoreEvidence(
        val eventsByOrder: Map<String, List<String>>,
        val paymentFailureCount: Int,
        val releasedReservationCount: Int,
        val startedIdempotencyCount: Int,
    )

    fun seedDefaults() {
        mutate { state ->
            val carts = childObject(state, "carts")
            putCart(carts, CartSeed("cart-success",
                listOf(OrderLineInput("sku-keyboard", 1), OrderLineInput("sku-mouse", 1)), 120.00, "USD"))
            putCart(carts, CartSeed("cart-inventory-fail",
                listOf(OrderLineInput("sku-soldout", 2)), 88.00, "USD"))
            putCart(carts, CartSeed("cart-payment-fail",
                listOf(OrderLineInput("sku-headset", 1)), 64.00, "USD"))

            val inventory = childObject(state, "inventory")
            putIfAbsentInt(inventory, "sku-keyboard", 5)
            putIfAbsentInt(inventory, "sku-mouse", 5)
            putIfAbsentInt(inventory, "sku-headset", 3)
            putIfAbsentInt(inventory, "sku-soldout", 1)

            val payments = childObject(state, "paymentMethods")
            putPaymentMethod(payments, PaymentMethodSeed("pm-ok", true, null))
            putPaymentMethod(payments, PaymentMethodSeed("pm-decline", false, "payment declined"))

            val addresses = childObject(state, "shippingAddresses")
            addresses.put("addr-home", true)
            addresses.put("addr-office", true)
            null
        }
    }

    fun findCart(cartId: String): CartSeed? =
        read { state ->
            val carts = state.get("carts")
            if (carts == null || !carts.has(cartId)) null else readCart(carts.get(cartId))
        }

    fun shippingAddressExists(shippingAddressId: String): Boolean =
        read { state -> state.get("shippingAddresses")?.has(shippingAddressId) == true }

    fun paymentMethodExists(paymentMethodId: String): Boolean =
        read { state -> state.get("paymentMethods")?.has(paymentMethodId) == true }

    fun findIdempotency(idempotencyKey: String): IdempotencyMapping? =
        read { state -> findMapping(state, idempotencyKey) }

    fun reserveIdempotency(idempotencyKey: String, ownerInstanceId: String): IdempotencyMapping =
        mutate { state ->
            val existing = findMapping(state, idempotencyKey)
            if (existing != null) {
                existing
            } else {
                val next = state.path("nextOrderSequence").asLong(0) + 1
                state.put("nextOrderSequence", next)
                val orderId = "order-%04d".format(next)
                val mapping = IdempotencyMapping(idempotencyKey, orderId, ownerInstanceId, false)
                writeMapping(state, mapping)
                mapping
            }
        }

    fun createPendingMapping(idempotencyKey: String, orderId: String, ownerInstanceId: String) {
        mutate { state ->
            writeMapping(state, IdempotencyMapping(idempotencyKey, orderId, ownerInstanceId, false))
            null
        }
    }

    fun markIdempotencyStarted(idempotencyKey: String) {
        mutate { state ->
            findMapping(state, idempotencyKey)?.let { mapping ->
                writeMapping(state, mapping.copy(started = true))
            }
            null
        }
    }

    fun saveOrderPaymentMethod(orderId: String, paymentMethodId: String) {
        mutate { state ->
            childObject(state, "orderPaymentMethods").put(orderId, paymentMethodId)
            null
        }
    }

    fun reserveInventory(orderId: String, lines: List<OrderLineInput>): ReserveInventoryResult =
        mutate { state ->
            val inventory = childObject(state, "inventory")
            for (line in lines) {
                val available = inventory.path(line.sku).asInt(0)
                if (line.quantity > available) {
                    return@mutate ReserveInventoryResult(false, null, "inventory unavailable for ${line.sku}")
                }
            }
            for (line in lines) {
                val available = inventory.path(line.sku).asInt(0)
                inventory.put(line.sku, available - line.quantity)
            }
            val reservationId = "reservation-$orderId"
            childObject(state, "reservations").put(reservationId, orderId)
            ReserveInventoryResult(true, reservationId, null)
        }

    fun releaseInventory(orderId: String, reservationId: String, reason: String) {
        mutate { state ->
            childObject(state, "releasedReservations").put(reservationId, reason)
            null
        }
    }

    fun authorizePayment(
        orderId: String,
        paymentMethodId: String,
        amount: Double,
        currency: String,
    ): AuthorizePaymentResult =
        mutate { state ->
            val method = state.get("paymentMethods")?.get(paymentMethodId)
            val shouldAuthorize = method?.path("shouldAuthorize")?.asBoolean(false) == true
            if (!shouldAuthorize) {
                val reason = if (method == null) {
                    "payment method missing"
                } else {
                    method.path("failureReason").asText("payment failed")
                }
                childObject(state, "paymentAttempts").put(orderId, reason)
                AuthorizePaymentResult(false, null, reason)
            } else {
                val paymentId = "payment-$orderId"
                childObject(state, "payments").put(paymentId, "%.2f %s".format(amount, currency))
                AuthorizePaymentResult(true, paymentId, null)
            }
        }

    fun readEvents(orderId: String): List<StoredEvent> =
        read { state ->
            val events = mutableListOf<StoredEvent>()
            val streams = state.get("events")
            if (streams != null && streams.has(orderId)) {
                for (node in streams.get(orderId)) {
                    events.add(readStoredEvent(node))
                }
            }
            events.sortedBy { it.version }
        }

    fun appendEvents(orderId: String, expectedVersion: Long, events: List<StoredEvent>): Long =
        mutate { state ->
            val streams = childObject(state, "events")
            val stream = if (streams.has(orderId)) {
                streams.get(orderId) as ArrayNode
            } else {
                streams.putArray(orderId)
            }
            var currentVersion = 0L
            val existing = mutableListOf<StoredEvent>()
            for (node in stream) {
                val stored = readStoredEvent(node)
                existing.add(stored)
                currentVersion = maxOf(currentVersion, stored.version)
            }
            if (currentVersion != expectedVersion) {
                throw IllegalStateException(
                    "Order stream version mismatch for '$orderId': expected $expectedVersion " +
                        "but found $currentVersion.",
                )
            }
            for (event in events) {
                if (isDuplicate(existing, event)) {
                    continue
                }
                val version = ++currentVersion
                val appended = event.copy(orderId = orderId, version = version)
                stream.add(writeStoredEvent(appended))
                existing.add(appended)
            }
            currentVersion
        }

    private fun isDuplicate(existing: List<StoredEvent>, candidate: StoredEvent): Boolean {
        when (candidate.eventType) {
            "OrderStartedEvent" -> {
                for (stored in existing) {
                    if (stored.eventType == "OrderStartedEvent" &&
                        candidate.sourceCommandId != null &&
                        candidate.sourceCommandId == stored.sourceCommandId
                    ) {
                        return true
                    }
                }
            }
            "InventoryReservedEvent" -> {
                val reservationId = candidate.payload.path("reservationId").asText(null)
                for (stored in existing) {
                    if (stored.eventType == "InventoryReservedEvent" &&
                        reservationId != null &&
                        reservationId == stored.payload.path("reservationId").asText(null)
                    ) {
                        return true
                    }
                }
            }
            "PaymentAuthorizedEvent" -> {
                val paymentId = candidate.payload.path("paymentId").asText(null)
                for (stored in existing) {
                    if (stored.eventType == "PaymentAuthorizedEvent" &&
                        paymentId != null &&
                        paymentId == stored.payload.path("paymentId").asText(null)
                    ) {
                        return true
                    }
                }
            }
            "OrderConfirmedEvent", "OrderFailedEvent" -> {
                for (stored in existing) {
                    if (candidate.eventType == stored.eventType) {
                        return true
                    }
                }
            }
        }
        return false
    }

    fun findReadModel(orderId: String): OrderState? =
        read { state ->
            val models = state.get("readModels")
            if (models == null || !models.has(orderId)) null else readState(models.get(orderId))
        }

    fun saveReadModel(orderState: OrderState) {
        mutate { state ->
            childObject(state, "readModels").set<JsonNode>(orderState.orderId, writeState(orderState))
            null
        }
    }

    fun deleteReadModel(orderId: String): Boolean =
        mutate { state ->
            val models = childObject(state, "readModels")
            if (!models.has(orderId)) {
                false
            } else {
                models.remove(orderId)
                true
            }
        }

    fun evidence(orderIds: List<String>): StoreEvidence =
        read { state ->
            val eventsByOrder = LinkedHashMap<String, List<String>>()
            val streams = state.get("events")
            for (orderId in orderIds) {
                val types = mutableListOf<String>()
                if (streams != null && streams.has(orderId)) {
                    val sorted = mutableListOf<StoredEvent>()
                    for (node in streams.get(orderId)) {
                        sorted.add(readStoredEvent(node))
                    }
                    sorted.sortBy { it.version }
                    for (stored in sorted) {
                        types.add(stored.eventType)
                    }
                }
                eventsByOrder[orderId] = types
            }
            val paymentFailureCount = sizeOf(state.get("paymentAttempts"))
            val releasedReservationCount = sizeOf(state.get("releasedReservations"))
            var startedCount = 0
            val mappings = state.get("idempotency")
            if (mappings != null) {
                for (mapping in mappings) {
                    if (mapping.path("started").asBoolean(false)) {
                        startedCount++
                    }
                }
            }
            StoreEvidence(eventsByOrder, paymentFailureCount, releasedReservationCount, startedCount)
        }

    fun placeholder(orderId: String): OrderState = topology.failedPlaceholder(orderId)

    // ----- json plumbing -----

    private fun findMapping(state: JsonNode, idempotencyKey: String): IdempotencyMapping? {
        val mappings = state.get("idempotency")
        if (mappings == null || !mappings.has(idempotencyKey)) {
            return null
        }
        val node = mappings.get(idempotencyKey)
        return IdempotencyMapping(
            idempotencyKey,
            node.path("orderId").asText(),
            node.path("ownerInstanceId").asText(),
            node.path("started").asBoolean(false),
        )
    }

    private fun writeMapping(state: ObjectNode, mapping: IdempotencyMapping) {
        val mappings = childObject(state, "idempotency")
        val node = json.createObjectNode()
        node.put("orderId", mapping.orderId)
        node.put("ownerInstanceId", mapping.ownerInstanceId)
        node.put("started", mapping.started)
        mappings.set<JsonNode>(mapping.idempotencyKey, node)
    }

    private fun readCart(node: JsonNode): CartSeed {
        val lines = mutableListOf<OrderLineInput>()
        for (line in node.path("lines")) {
            lines.add(OrderLineInput(line.path("sku").asText(), line.path("quantity").asInt()))
        }
        return CartSeed(
            node.path("cartId").asText(),
            lines,
            node.path("amount").asDouble(),
            node.path("currency").asText(),
        )
    }

    private fun putCart(carts: ObjectNode, cart: CartSeed) {
        if (carts.has(cart.cartId)) {
            return
        }
        val node = json.createObjectNode()
        node.put("cartId", cart.cartId)
        val lines = node.putArray("lines")
        for (line in cart.lines) {
            val lineNode = json.createObjectNode()
            lineNode.put("sku", line.sku)
            lineNode.put("quantity", line.quantity)
            lines.add(lineNode)
        }
        node.put("amount", cart.amount)
        node.put("currency", cart.currency)
        carts.set<JsonNode>(cart.cartId, node)
    }

    private fun putPaymentMethod(methods: ObjectNode, seed: PaymentMethodSeed) {
        if (methods.has(seed.paymentMethodId)) {
            return
        }
        val node = json.createObjectNode()
        node.put("paymentMethodId", seed.paymentMethodId)
        node.put("shouldAuthorize", seed.shouldAuthorize)
        if (seed.failureReason != null) {
            node.put("failureReason", seed.failureReason)
        }
        methods.set<JsonNode>(seed.paymentMethodId, node)
    }

    private fun putIfAbsentInt(node: ObjectNode, key: String, value: Int) {
        if (!node.has(key)) {
            node.put(key, value)
        }
    }

    private fun readStoredEvent(node: JsonNode): StoredEvent =
        StoredEvent(
            node.path("eventId").asText(),
            if (node.path("sourceCommandId").isNull) null else node.path("sourceCommandId").asText(null),
            node.path("orderId").asText(),
            node.path("eventType").asText(),
            node.path("payload"),
            node.path("version").asLong(),
            node.path("createdAtUnixMs").asLong(),
        )

    private fun writeStoredEvent(event: StoredEvent): ObjectNode {
        val node = json.createObjectNode()
        node.put("eventId", event.eventId)
        if (event.sourceCommandId == null) {
            node.putNull("sourceCommandId")
        } else {
            node.put("sourceCommandId", event.sourceCommandId)
        }
        node.put("orderId", event.orderId)
        node.put("eventType", event.eventType)
        node.set<JsonNode>("payload", event.payload)
        node.put("version", event.version)
        node.put("createdAtUnixMs", event.createdAtUnixMs)
        return node
    }

    private fun readState(node: JsonNode): OrderState =
        OrderState(
            node.path("orderId").asText(),
            node.path("status").asText(),
            textOrNull(node, "shippingAddressId"),
            textOrNull(node, "reservationId"),
            textOrNull(node, "paymentId"),
            textOrNull(node, "reason"),
            if (node.has("amount") && !node.path("amount").isNull) node.path("amount").asDouble() else null,
            textOrNull(node, "currency"),
            node.path("updatedAtUnixMs").asLong(),
        )

    private fun writeState(state: OrderState): ObjectNode {
        val node = json.createObjectNode()
        node.put("orderId", state.orderId)
        node.put("status", state.status)
        putNullable(node, "shippingAddressId", state.shippingAddressId)
        putNullable(node, "reservationId", state.reservationId)
        putNullable(node, "paymentId", state.paymentId)
        putNullable(node, "reason", state.reason)
        if (state.amount == null) {
            node.putNull("amount")
        } else {
            node.put("amount", state.amount)
        }
        putNullable(node, "currency", state.currency)
        node.put("updatedAtUnixMs", state.updatedAtUnixMs)
        return node
    }

    private fun textOrNull(node: JsonNode, field: String): String? {
        val value = node.get(field)
        return if (value == null || value.isNull) null else value.asText()
    }

    private fun putNullable(node: ObjectNode, field: String, value: String?) {
        if (value == null) {
            node.putNull(field)
        } else {
            node.put(field, value)
        }
    }

    private fun sizeOf(node: JsonNode?): Int = node?.size() ?: 0

    private fun childObject(state: ObjectNode, field: String): ObjectNode {
        val child = state.get(field)
        return if (child is ObjectNode) child else state.putObject(field)
    }

    // ----- locked read / mutate -----

    private fun <T> read(reader: (ObjectNode) -> T): T = withState(reader, false)

    private fun <T> mutate(mutator: (ObjectNode) -> T): T = withState(mutator, true)

    private fun <T> withState(action: (ObjectNode) -> T, write: Boolean): T = synchronized(stateMutex) {
        FileChannel.open(lockFile, StandardOpenOption.CREATE, StandardOpenOption.WRITE).use { channel ->
            val lock = acquire(channel)
            try {
                val state = loadState()
                val result = action(state)
                if (write) {
                    saveState(state)
                }
                return result
            } finally {
                lock.release()
            }
        }
    }

    private fun acquire(channel: FileChannel): java.nio.channels.FileLock {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(30)
        var lock = channel.tryLock()
        while (lock == null && System.nanoTime() < deadline) {
            LockSupport.parkNanos(TimeUnit.MILLISECONDS.toNanos(10))
            lock = channel.tryLock()
        }
        return lock ?: throw IllegalStateException("Timed out acquiring commerce store lock.")
    }

    private fun loadState(): ObjectNode {
        if (!Files.exists(stateFile)) {
            return json.createObjectNode()
        }
        val bytes = Files.readAllBytes(stateFile)
        if (bytes.isEmpty()) {
            return json.createObjectNode()
        }
        val node = json.readTree(bytes)
        return if (node is ObjectNode) node else json.createObjectNode()
    }

    private fun saveState(state: ObjectNode) {
        Files.write(
            stateFile,
            json.writerWithDefaultPrettyPrinter().writeValueAsString(state).toByteArray(StandardCharsets.UTF_8),
            StandardOpenOption.CREATE,
            StandardOpenOption.TRUNCATE_EXISTING,
            StandardOpenOption.WRITE,
        )
    }
}
