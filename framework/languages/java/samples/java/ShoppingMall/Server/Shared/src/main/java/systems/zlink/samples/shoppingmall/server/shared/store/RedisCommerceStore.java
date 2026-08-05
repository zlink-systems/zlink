package systems.zlink.samples.shoppingmall.server.shared.store;

import com.fasterxml.jackson.databind.ObjectMapper;
import io.lettuce.core.RedisClient;
import io.lettuce.core.SetArgs;
import io.lettuce.core.api.StatefulRedisConnection;
import io.lettuce.core.api.sync.RedisCommands;
import java.math.BigDecimal;
import java.time.Instant;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.function.Function;
import java.util.concurrent.locks.LockSupport;
import systems.zlink.samples.shoppingmall.server.configuration.SampleTopology;
import systems.zlink.samples.shoppingmall.server.shared.domain.OrderDomain;
import systems.zlink.samples.shoppingmall.server.shared.domain.OrderProjection;
import systems.zlink.samples.shoppingmall.shared.contracts.Messages;

public final class RedisCommerceStore implements AutoCloseable {
    private final ObjectMapper json = new ObjectMapper().findAndRegisterModules();
    private final RedisClient client;
    private final StatefulRedisConnection<String, String> connection;
    private final RedisCommands<String, String> redis;
    private final String stateKey;
    private final String lockKey;

    public RedisCommerceStore(SampleTopology topology) {
        SampleTopology.Location location = topology.location();
        client = RedisClient.create(redisUri(location.redisEndpoint()));
        connection = client.connect();
        redis = connection.sync();
        stateKey = location.redisKeyPrefix() + "shoppingmall:commerce-state";
        lockKey = location.redisKeyPrefix() + "shoppingmall:commerce-state:lock";
    }

    public void seedDefaults() {
        withState(state -> {
            state.carts.putIfAbsent("cart-success", new Messages.CartSeed(
                "cart-success",
                List.of(
                    new Messages.OrderLineInput("sku-keyboard", 1),
                    new Messages.OrderLineInput("sku-mouse", 1)),
                new BigDecimal("120.00"),
                "USD"));
            state.carts.putIfAbsent("cart-inventory-fail", new Messages.CartSeed(
                "cart-inventory-fail",
                List.of(new Messages.OrderLineInput("sku-soldout", 2)),
                new BigDecimal("88.00"),
                "USD"));
            state.carts.putIfAbsent("cart-payment-fail", new Messages.CartSeed(
                "cart-payment-fail",
                List.of(new Messages.OrderLineInput("sku-headset", 1)),
                new BigDecimal("64.00"),
                "USD"));
            state.inventory.putIfAbsent("sku-keyboard", 5);
            state.inventory.putIfAbsent("sku-mouse", 5);
            state.inventory.putIfAbsent("sku-headset", 5);
            state.inventory.putIfAbsent("sku-soldout", 0);
            state.paymentMethods.putIfAbsent("pm-ok", new Messages.PaymentMethodSeed("pm-ok", true, null));
            state.paymentMethods.putIfAbsent(
                "pm-decline",
                new Messages.PaymentMethodSeed("pm-decline", false, "payment declined"));
            state.shippingAddresses.add("addr-home");
            state.shippingAddresses.add("addr-office");
            return null;
        });
    }

    public Messages.CartSeed getCart(String cartId) {
        return withState(state -> require(state.carts.get(cartId), "Cart '" + cartId + "' does not exist."));
    }

    public void validateShippingAddress(String shippingAddressId) {
        withState(state -> {
            if (!state.shippingAddresses.contains(shippingAddressId)) {
                throw new IllegalStateException("Shipping address '" + shippingAddressId + "' does not exist.");
            }
            return null;
        });
    }

    public Messages.PaymentMethodSeed getPaymentMethod(String paymentMethodId) {
        return withState(state -> require(
            state.paymentMethods.get(paymentMethodId),
            "Payment method '" + paymentMethodId + "' does not exist."));
    }

    public OrderDomain.IdempotencyMapping findIdempotency(String idempotencyKey) {
        return withState(state -> state.idempotency.get(idempotencyKey));
    }

    public OrderDomain.IdempotencyMapping reserveIdempotency(String idempotencyKey, String ownerInstanceId) {
        return withState(state -> {
            OrderDomain.IdempotencyMapping existing = state.idempotency.get(idempotencyKey);
            if (existing != null) {
                return existing;
            }
            String orderId = "order-" + String.format("%04d", ++state.nextOrderSequence);
            OrderDomain.IdempotencyMapping created =
                new OrderDomain.IdempotencyMapping(idempotencyKey, orderId, ownerInstanceId, false);
            state.idempotency.put(idempotencyKey, created);
            return created;
        });
    }

    public void createPendingMapping(String idempotencyKey, String orderId, String ownerInstanceId) {
        withState(state -> {
            state.idempotency.put(
                idempotencyKey,
                new OrderDomain.IdempotencyMapping(idempotencyKey, orderId, ownerInstanceId, false));
            return null;
        });
    }

    public void markIdempotencyStarted(String idempotencyKey) {
        withState(state -> {
            OrderDomain.IdempotencyMapping current = state.idempotency.get(idempotencyKey);
            if (current != null) {
                state.idempotency.put(
                    idempotencyKey,
                    new OrderDomain.IdempotencyMapping(
                        current.idempotencyKey(),
                        current.orderId(),
                        current.ownerInstanceId(),
                        true));
            }
            return null;
        });
    }

    public void saveOrderPaymentMethod(String orderId, String paymentMethodId) {
        withState(state -> {
            state.orderPaymentMethods.put(orderId, paymentMethodId);
            return null;
        });
    }

    public String getOrderPaymentMethod(String orderId) {
        return withState(state -> require(
            state.orderPaymentMethods.get(orderId),
            "Payment method for order '" + orderId + "' does not exist."));
    }

    public OrderDomain.ReserveInventoryResult reserveInventory(
        String orderId,
        String reservationId,
        List<Messages.OrderLineInput> lines) {
        return withState(state -> {
            OrderDomain.InventoryReservation existing = state.reservations.get(reservationId);
            if (existing != null && existing.orderId().equals(orderId)) {
                return new OrderDomain.ReserveInventoryResult(true, reservationId, null);
            }
            for (Messages.OrderLineInput line : lines) {
                int available = state.inventory.getOrDefault(line.sku(), 0);
                if (available < line.quantity()) {
                    return new OrderDomain.ReserveInventoryResult(
                        false,
                        null,
                        "inventory unavailable for " + line.sku());
                }
            }
            for (Messages.OrderLineInput line : lines) {
                state.inventory.put(line.sku(), state.inventory.getOrDefault(line.sku(), 0) - line.quantity());
            }
            state.reservations.put(
                reservationId,
                new OrderDomain.InventoryReservation(
                    orderId,
                    lines.stream()
                        .map(line -> new OrderDomain.InventoryReservationLine(line.sku(), line.quantity()))
                        .toList()));
            return new OrderDomain.ReserveInventoryResult(true, reservationId, null);
        });
    }

    public void releaseInventory(String orderId, String reservationId, String reason) {
        withState(state -> {
            if (state.releasedReservations.containsKey(reservationId)) {
                return null;
            }
            OrderDomain.InventoryReservation reservation = state.reservations.get(reservationId);
            if (reservation != null && reservation.orderId().equals(orderId)) {
                for (OrderDomain.InventoryReservationLine line : reservation.lines()) {
                    state.inventory.put(line.sku(), state.inventory.getOrDefault(line.sku(), 0) + line.quantity());
                }
            }
            state.releasedReservations.put(reservationId, reason);
            return null;
        });
    }

    public OrderDomain.AuthorizePaymentResult authorizePayment(
        String orderId,
        String paymentId,
        String paymentMethodId,
        BigDecimal amount,
        String currency) {
        return withState(state -> {
            if (state.payments.containsKey(paymentId)) {
                return new OrderDomain.AuthorizePaymentResult(true, paymentId, null);
            }
            if (state.paymentAttempts.containsKey(orderId)) {
                return new OrderDomain.AuthorizePaymentResult(false, null, state.paymentAttempts.get(orderId));
            }
            Messages.PaymentMethodSeed method = state.paymentMethods.get(paymentMethodId);
            if (method == null || !method.shouldAuthorize()) {
                String reason = method == null || method.failureReason() == null
                    ? "payment failed"
                    : method.failureReason();
                state.paymentAttempts.put(orderId, reason);
                return new OrderDomain.AuthorizePaymentResult(false, null, reason);
            }
            state.payments.put(paymentId, amount + " " + currency);
            return new OrderDomain.AuthorizePaymentResult(true, paymentId, null);
        });
    }

    public List<OrderDomain.StoredOrderEvent> readEvents(String orderId) {
        return withState(state -> state.events.getOrDefault(orderId, List.of())
            .stream()
            .map(EventRecord::toStored)
            .toList());
    }

    public void appendEvents(String orderId, long expectedVersion, List<OrderDomain.OrderEvent> events) {
        if (events.isEmpty()) {
            return;
        }
        withState(state -> {
            List<EventRecord> stream = state.events.computeIfAbsent(orderId, ignored -> new ArrayList<>());
            long currentVersion = stream.isEmpty() ? 0 : stream.getLast().version;
            if (currentVersion != expectedVersion) {
                throw new IllegalStateException(
                    "Order stream version conflict: order=" + orderId
                        + " expected=" + expectedVersion
                        + " actual=" + currentVersion);
            }
            for (OrderDomain.OrderEvent event : events) {
                if (isDuplicate(stream, event)) {
                    continue;
                }
                stream.add(EventRecord.from(event, ++currentVersion));
            }
            return null;
        });
    }

    public Messages.OrderState findProjection(String orderId) {
        return withState(state -> state.readModels.get(orderId));
    }

    public void saveProjection(Messages.OrderState projection) {
        withState(state -> {
            state.readModels.put(projection.orderId(), projection);
            return null;
        });
    }

    public void deleteProjection(String orderId) {
        withState(state -> {
            state.readModels.remove(orderId);
            return null;
        });
    }

    public OrderDomain.StoreEvidence evidence(List<String> orderIds) {
        return withState(state -> {
            Map<String, List<String>> eventsByOrder = new LinkedHashMap<>();
            for (String orderId : orderIds) {
                eventsByOrder.put(
                    orderId,
                    state.events.getOrDefault(orderId, List.of())
                        .stream()
                        .map(record -> record.eventType)
                        .toList());
            }
            long paymentFailures = state.events.values().stream()
                .flatMap(List::stream)
                .filter(record -> "PaymentFailedEvent".equals(record.eventType))
                .count();
            return new OrderDomain.StoreEvidence(
                eventsByOrder,
                (int) paymentFailures,
                state.releasedReservations.size(),
                (int) state.idempotency.values().stream().filter(OrderDomain.IdempotencyMapping::started).count());
        });
    }

    public Messages.OrderState rebuildProjection(String orderId) {
        Messages.OrderState rebuilt = OrderProjection.fold(readEvents(orderId));
        if (rebuilt == null) {
            throw new IllegalStateException("Order '" + orderId + "' does not exist.");
        }
        saveProjection(rebuilt);
        return rebuilt;
    }

    public static String eventId(String prefix, String orderId) {
        return prefix + "-" + orderId + "-" + UUID.randomUUID();
    }

    public static long nowMs() {
        return Instant.now().toEpochMilli();
    }

    @Override
    public void close() {
        connection.close();
        client.shutdown();
    }

    private <T> T withState(Function<CommerceState, T> operation) {
        String token = UUID.randomUUID().toString();
        long deadline = System.currentTimeMillis() + 10_000;
        while (!"OK".equals(redis.set(lockKey, token, SetArgs.Builder.nx().px(10_000)))) {
            if (System.currentTimeMillis() >= deadline) {
                throw new IllegalStateException("Timed out waiting for ShoppingMall Redis state lock.");
            }
            if (Thread.currentThread().isInterrupted()) {
                throw new IllegalStateException("Interrupted while waiting for ShoppingMall Redis state lock.");
            }
            LockSupport.parkNanos(10_000_000L);
        }
        try {
            CommerceState state = readState();
            T value = operation.apply(state);
            writeState(state);
            return value;
        } finally {
            if (token.equals(redis.get(lockKey))) {
                redis.del(lockKey);
            }
        }
    }

    private CommerceState readState() {
        String value = redis.get(stateKey);
        if (value == null || value.isBlank()) {
            return new CommerceState();
        }
        try {
            return json.readValue(value, CommerceState.class);
        } catch (Exception ex) {
            throw new IllegalStateException("Could not read ShoppingMall sample state.", ex);
        }
    }

    private void writeState(CommerceState state) {
        try {
            redis.set(stateKey, json.writeValueAsString(state));
        } catch (Exception ex) {
            throw new IllegalStateException("Could not write ShoppingMall sample state.", ex);
        }
    }

    private boolean isDuplicate(List<EventRecord> stream, OrderDomain.OrderEvent event) {
        if (event instanceof OrderDomain.OrderStartedEvent started) {
            return stream.stream().anyMatch(record ->
                "OrderStartedEvent".equals(record.eventType)
                    && started.sourceCommandId().equals(record.sourceCommandId));
        }
        return stream.stream().anyMatch(record -> record.eventId.equals(event.eventId())
            || semanticKey(record).equals(semanticKey(event)));
    }

    private static String semanticKey(EventRecord record) {
        return record.eventType + ":" + record.reservationId + ":" + record.paymentId + ":" + record.reason;
    }

    private static String semanticKey(OrderDomain.OrderEvent event) {
        return switch (event) {
            case OrderDomain.InventoryReservedEvent value -> value.eventType() + ":" + value.reservationId();
            case OrderDomain.InventoryReservationFailedEvent value -> value.eventType() + ":" + value.reason();
            case OrderDomain.PaymentAuthorizedEvent value -> value.eventType() + ":" + value.paymentId();
            case OrderDomain.PaymentFailedEvent value -> value.eventType() + ":" + value.reason();
            case OrderDomain.InventoryReleasedEvent value -> value.eventType() + ":" + value.reservationId();
            case OrderDomain.OrderConfirmedEvent value -> value.eventType();
            case OrderDomain.OrderFailedEvent value -> value.eventType() + ":" + value.reason();
            case OrderDomain.OrderStartedEvent value -> value.eventType() + ":" + value.sourceCommandId();
        };
    }

    private static <T> T require(T value, String message) {
        if (value == null) {
            throw new IllegalStateException(message);
        }
        return value;
    }

    private static String redisUri(String endpoint) {
        return endpoint.startsWith("redis://") ? endpoint : "redis://" + endpoint;
    }

    public static final class CommerceState {
        public Map<String, Messages.CartSeed> carts = new HashMap<>();
        public Map<String, Integer> inventory = new HashMap<>();
        public Map<String, Messages.PaymentMethodSeed> paymentMethods = new HashMap<>();
        public List<String> shippingAddresses = new ArrayList<>();
        public Map<String, OrderDomain.IdempotencyMapping> idempotency = new HashMap<>();
        public Map<String, String> orderPaymentMethods = new HashMap<>();
        public Map<String, OrderDomain.InventoryReservation> reservations = new HashMap<>();
        public Map<String, String> releasedReservations = new HashMap<>();
        public Map<String, String> payments = new HashMap<>();
        public Map<String, String> paymentAttempts = new HashMap<>();
        public Map<String, List<EventRecord>> events = new HashMap<>();
        public Map<String, Messages.OrderState> readModels = new HashMap<>();
        public int nextOrderSequence;
    }

    public static final class EventRecord {
        public String eventId;
        public String sourceCommandId;
        public String orderId;
        public String eventType;
        public String cartId;
        public String shippingAddressId;
        public List<Messages.OrderLineInput> lines;
        public BigDecimal amount;
        public String currency;
        public String reservationId;
        public String paymentId;
        public String reason;
        public long version;
        public long createdAtUnixMs;

        public static EventRecord from(OrderDomain.OrderEvent event, long version) {
            EventRecord record = new EventRecord();
            record.eventId = event.eventId();
            record.orderId = event.orderId();
            record.eventType = event.eventType();
            record.version = version;
            record.createdAtUnixMs = event.createdAtUnixMs();
            switch (event) {
                case OrderDomain.OrderStartedEvent value -> {
                    record.sourceCommandId = value.sourceCommandId();
                    record.cartId = value.cartId();
                    record.shippingAddressId = value.shippingAddressId();
                    record.lines = value.lines();
                    record.amount = value.amount();
                    record.currency = value.currency();
                }
                case OrderDomain.InventoryReservedEvent value -> record.reservationId = value.reservationId();
                case OrderDomain.InventoryReservationFailedEvent value -> record.reason = value.reason();
                case OrderDomain.PaymentAuthorizedEvent value -> record.paymentId = value.paymentId();
                case OrderDomain.PaymentFailedEvent value -> record.reason = value.reason();
                case OrderDomain.InventoryReleasedEvent value -> {
                    record.reservationId = value.reservationId();
                    record.reason = value.reason();
                }
                case OrderDomain.OrderConfirmedEvent ignored -> {
                }
                case OrderDomain.OrderFailedEvent value -> record.reason = value.reason();
            }
            return record;
        }

        public OrderDomain.StoredOrderEvent toStored() {
            return new OrderDomain.StoredOrderEvent(
                eventId,
                sourceCommandId,
                orderId,
                eventType,
                toEvent(),
                version,
                createdAtUnixMs);
        }

        private OrderDomain.OrderEvent toEvent() {
            return switch (eventType) {
                case "OrderStartedEvent" -> new OrderDomain.OrderStartedEvent(
                    eventId,
                    sourceCommandId,
                    orderId,
                    cartId,
                    shippingAddressId,
                    lines == null ? List.of() : lines,
                    amount,
                    currency,
                    createdAtUnixMs);
                case "InventoryReservedEvent" ->
                    new OrderDomain.InventoryReservedEvent(eventId, orderId, reservationId, createdAtUnixMs);
                case "InventoryReservationFailedEvent" ->
                    new OrderDomain.InventoryReservationFailedEvent(eventId, orderId, reason, createdAtUnixMs);
                case "PaymentAuthorizedEvent" ->
                    new OrderDomain.PaymentAuthorizedEvent(eventId, orderId, paymentId, createdAtUnixMs);
                case "PaymentFailedEvent" ->
                    new OrderDomain.PaymentFailedEvent(eventId, orderId, reason, createdAtUnixMs);
                case "InventoryReleasedEvent" ->
                    new OrderDomain.InventoryReleasedEvent(eventId, orderId, reservationId, reason, createdAtUnixMs);
                case "OrderConfirmedEvent" -> new OrderDomain.OrderConfirmedEvent(eventId, orderId, createdAtUnixMs);
                case "OrderFailedEvent" -> new OrderDomain.OrderFailedEvent(eventId, orderId, reason, createdAtUnixMs);
                default -> throw new IllegalStateException("Unknown order event type: " + eventType);
            };
        }
    }
}
