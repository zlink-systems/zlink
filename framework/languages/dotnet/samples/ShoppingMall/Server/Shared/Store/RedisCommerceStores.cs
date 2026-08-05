using System.Text.Json;
using ShoppingMall.Server.Configuration;
using ShoppingMall.Server.Shared.Domain;
using ShoppingMall.Server.Shared.Ports.Outbound;
using ShoppingMall.Shared.Contracts;
using StackExchange.Redis;

namespace ShoppingMall.Server.Shared.Store;

public sealed class RedisCommerceStores :
    IOrderEventStore,
    IOrderReadModelStore,
    ICommerceStateStore,
    IAsyncDisposable
{
    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web)
    {
        WriteIndented = true
    };

    private readonly IDatabase _database;
    private readonly string _lockKey;
    private readonly ConnectionMultiplexer _redis;
    private readonly string _stateKey;

    public RedisCommerceStores(SampleTopology topology)
    {
        _redis = ConnectionMultiplexer.Connect(topology.RedisEndpoint);
        _database = _redis.GetDatabase();
        _stateKey = $"{topology.RedisKeyPrefix}shoppingmall:commerce-state";
        _lockKey = $"{topology.RedisKeyPrefix}shoppingmall:commerce-state:lock";
    }

    public async ValueTask DisposeAsync()
    {
        await _redis.CloseAsync().ConfigureAwait(false);
        _redis.Dispose();
    }

    public async ValueTask<CartSeed> GetCartAsync(
        string cartId,
        CancellationToken cancellationToken)
    {
        return await WithStateAsync(
            state => state.Carts.TryGetValue(cartId, out var current)
                ? current
                : throw new InvalidOperationException($"Cart '{cartId}' does not exist."),
            cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask ValidateShippingAddressAsync(
        string shippingAddressId,
        CancellationToken cancellationToken)
    {
        await WithStateAsync(state =>
        {
            if (!state.ShippingAddresses.Contains(shippingAddressId))
                throw new InvalidOperationException($"Shipping address '{shippingAddressId}' does not exist.");
        }, cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask<PaymentMethodSeed> GetPaymentMethodAsync(
        string paymentMethodId,
        CancellationToken cancellationToken)
    {
        return await WithStateAsync(
            state => state.PaymentMethods.TryGetValue(paymentMethodId, out var current)
                ? current
                : throw new InvalidOperationException($"Payment method '{paymentMethodId}' does not exist."),
            cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask<IdempotencyMapping?> FindIdempotencyAsync(
        string idempotencyKey,
        CancellationToken cancellationToken)
    {
        return await WithStateAsync(
            state => state.Idempotency.GetValueOrDefault(idempotencyKey),
            cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask<IdempotencyMapping> ReserveIdempotencyAsync(
        string idempotencyKey,
        CancellationToken cancellationToken)
    {
        return await WithStateAsync(state =>
        {
            if (state.Idempotency.TryGetValue(idempotencyKey, out var existing)) return existing;

            var orderId = $"order-{++state.NextOrderSequence:0000}";
            var created = new IdempotencyMapping(idempotencyKey, orderId, false);
            state.Idempotency[idempotencyKey] = created;
            return created;
        }, cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask MarkIdempotencyStartedAsync(
        string idempotencyKey,
        CancellationToken cancellationToken)
    {
        await WithStateAsync(state =>
        {
            var mapping = state.Idempotency[idempotencyKey];
            state.Idempotency[idempotencyKey] = mapping with { Started = true };
        }, cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask CreatePendingMappingAsync(
        string idempotencyKey,
        string orderId,
        CancellationToken cancellationToken)
    {
        await WithStateAsync(state =>
        {
            state.Idempotency[idempotencyKey] = new IdempotencyMapping(
                idempotencyKey,
                orderId,
                false);
        }, cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask SaveOrderPaymentMethodAsync(
        string orderId,
        string paymentMethodId,
        CancellationToken cancellationToken)
    {
        await WithStateAsync(
            state => state.OrderPaymentMethods[orderId] = paymentMethodId,
            cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask<string> GetOrderPaymentMethodAsync(
        string orderId,
        CancellationToken cancellationToken)
    {
        return await WithStateAsync(
            state => state.OrderPaymentMethods.TryGetValue(orderId, out var current)
                ? current
                : throw new InvalidOperationException($"Payment method for order '{orderId}' does not exist."),
            cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask<ReserveInventoryResult> ReserveInventoryAsync(
        ReserveInventoryCommand command,
        CancellationToken cancellationToken)
    {
        return await WithStateAsync(state =>
        {
            if (state.Reservations.TryGetValue(command.ReservationId, out var existing)
                && string.Equals(existing.OrderId, command.OrderId, StringComparison.Ordinal))
                return new ReserveInventoryResult(true, command.ReservationId, null);

            foreach (var line in command.Lines)
            {
                var available = state.Inventory.GetValueOrDefault(line.Sku);
                if (available < line.Quantity)
                    return new ReserveInventoryResult(false, null, $"inventory unavailable for {line.Sku}");
            }

            foreach (var line in command.Lines) state.Inventory[line.Sku] -= line.Quantity;

            state.Reservations[command.ReservationId] = new InventoryReservation(
                command.OrderId,
                command.Lines.Select(static line => new InventoryReservationLine(line.Sku, line.Quantity)).ToArray());
            return new ReserveInventoryResult(true, command.ReservationId, null);
        }, cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask<ReleaseInventoryResult> ReleaseInventoryAsync(
        ReleaseInventoryCommand command,
        CancellationToken cancellationToken)
    {
        return await WithStateAsync(state =>
        {
            if (state.ReleasedReservations.ContainsKey(command.ReservationId))
                return new ReleaseInventoryResult(true);
            if (state.Reservations.TryGetValue(command.ReservationId, out var reservation)
                && string.Equals(reservation.OrderId, command.OrderId, StringComparison.Ordinal))
            {
                foreach (var line in reservation.Lines)
                    state.Inventory[line.Sku] = state.Inventory.GetValueOrDefault(line.Sku) + line.Quantity;
            }

            state.ReleasedReservations[command.ReservationId] = command.Reason;
            return new ReleaseInventoryResult(true);
        }, cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask<AuthorizePaymentResult> AuthorizePaymentAsync(
        AuthorizePaymentCommand command,
        CancellationToken cancellationToken)
    {
        return await WithStateAsync(state =>
        {
            if (state.Payments.ContainsKey(command.PaymentId))
                return new AuthorizePaymentResult(true, command.PaymentId, null);

            if (state.PaymentAttempts.TryGetValue(command.OrderId, out var existingFailure))
                return new AuthorizePaymentResult(false, null, existingFailure);

            var method = state.PaymentMethods[command.PaymentMethodId];
            if (!method.ShouldAuthorize)
            {
                state.PaymentAttempts[command.OrderId] = method.FailureReason ?? "payment failed";
                return new AuthorizePaymentResult(false, null, state.PaymentAttempts[command.OrderId]);
            }

            state.Payments[command.PaymentId] = $"{command.Amount:0.00} {command.Currency}";
            return new AuthorizePaymentResult(true, command.PaymentId, null);
        }, cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask<IReadOnlyList<StoredOrderEvent>> ReadAsync(
        string orderId,
        CancellationToken cancellationToken)
    {
        return await WithStateAsync<IReadOnlyList<StoredOrderEvent>>(
            state => state.Events.TryGetValue(orderId, out var current)
                ? current.OrderBy(static item => item.Version).ToArray()
                : [],
            cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask AppendAsync(
        string orderId,
        long expectedVersion,
        IReadOnlyList<OrderDomainEvent> events,
        CancellationToken cancellationToken)
    {
        await WithStateAsync(state =>
        {
            if (!state.Events.TryGetValue(orderId, out var stream))
            {
                stream = [];
                state.Events[orderId] = stream;
            }

            var currentVersion = stream.Count == 0 ? 0 : stream.Max(static item => item.Version);
            if (currentVersion != expectedVersion)
                throw new OrderStreamVersionConflictException(orderId, expectedVersion, currentVersion);

            foreach (var domainEvent in events)
            {
                var sourceCommandId = domainEvent is OrderStartedEvent started ? started.SourceCommandId : null;
                if (sourceCommandId is not null
                    && stream.Any(item => string.Equals(item.SourceCommandId, sourceCommandId, StringComparison.Ordinal)
                                          && item.EventType == nameof(OrderStartedEvent)))
                    continue;

                if (IsDuplicateSemanticEvent(stream, domainEvent)) continue;

                stream.Add(new StoredOrderEvent(
                    domainEvent.EventId,
                    sourceCommandId,
                    orderId,
                    domainEvent.GetType().Name,
                    StoredOrderEventPayload.Encode(domainEvent),
                    ++currentVersion,
                    domainEvent.CreatedAtUnixMs));
            }
        }, cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask<OrderProjectionState?> FindAsync(
        string orderId,
        CancellationToken cancellationToken)
    {
        return await WithStateAsync(
            state => state.ReadModels.GetValueOrDefault(orderId),
            cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask SaveAsync(
        OrderProjectionState state,
        CancellationToken cancellationToken)
    {
        await WithStateAsync(
            current => current.ReadModels[state.OrderId] = state,
            cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask DeleteAsync(
        string orderId,
        CancellationToken cancellationToken)
    {
        await WithStateAsync(
            state => state.ReadModels.Remove(orderId),
            cancellationToken).ConfigureAwait(false);
    }

    public void SeedDefaults()
    {
        WithStateAsync(state =>
        {
            state.Carts.TryAdd("cart-success", new CartSeed(
                "cart-success",
                [new OrderLineInput("sku-keyboard", 1), new OrderLineInput("sku-mouse", 1)],
                120.00m,
                "USD"));
            state.Carts.TryAdd("cart-inventory-fail", new CartSeed(
                "cart-inventory-fail",
                [new OrderLineInput("sku-soldout", 2)],
                88.00m,
                "USD"));
            state.Carts.TryAdd("cart-payment-fail", new CartSeed(
                "cart-payment-fail",
                [new OrderLineInput("sku-headset", 1)],
                64.00m,
                "USD"));

            state.Inventory.TryAdd("sku-keyboard", 5);
            state.Inventory.TryAdd("sku-mouse", 5);
            state.Inventory.TryAdd("sku-headset", 3);
            state.Inventory.TryAdd("sku-soldout", 1);

            state.PaymentMethods.TryAdd("pm-ok", new PaymentMethodSeed("pm-ok", true, null));
            state.PaymentMethods.TryAdd("pm-decline", new PaymentMethodSeed("pm-decline", false, "payment declined"));

            state.ShippingAddresses.Add("addr-home");
            state.ShippingAddresses.Add("addr-office");
        }, CancellationToken.None).AsTask().GetAwaiter().GetResult();
    }

    public async ValueTask<StoreEvidence> EvidenceAsync(
        IReadOnlyList<string> orderIds,
        CancellationToken cancellationToken)
    {
        return await WithStateAsync(state =>
        {
            var events = orderIds.ToDictionary(
                static orderId => orderId,
                orderId => state.Events.TryGetValue(orderId, out var stream)
                    ? stream.Select(static item => item.EventType).ToArray()
                    : [],
                StringComparer.Ordinal);
            return new StoreEvidence(
                events,
                state.PaymentAttempts.Count,
                state.ReleasedReservations.Count,
                state.Idempotency.Values.Count(static item => item.Started));
        }, cancellationToken).ConfigureAwait(false);
    }

    private static bool IsDuplicateSemanticEvent(
        IReadOnlyList<StoredOrderEvent> stream,
        OrderDomainEvent domainEvent)
    {
        return domainEvent switch
        {
            InventoryReservedEvent reserved => stream.Any(item =>
                item.Decode() is InventoryReservedEvent current
                && current.ReservationId == reserved.ReservationId),
            PaymentAuthorizedEvent paid => stream.Any(item =>
                item.Decode() is PaymentAuthorizedEvent current
                && current.PaymentId == paid.PaymentId),
            OrderConfirmedEvent => stream.Any(static item => item.Decode() is OrderConfirmedEvent),
            OrderFailedEvent => stream.Any(static item => item.Decode() is OrderFailedEvent),
            _ => false
        };
    }

    private async ValueTask<string> AcquireLockAsync(CancellationToken cancellationToken)
    {
        var token = $"{Environment.ProcessId}:{Guid.NewGuid():N}";
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (await _database.LockTakeAsync(_lockKey, token, TimeSpan.FromSeconds(20)).ConfigureAwait(false))
                return token;
            await Task.Delay(10, cancellationToken).ConfigureAwait(false);
        }
    }

    private ValueTask ReleaseLockAsync(string token)
    {
        return new ValueTask(_database.LockReleaseAsync(_lockKey, token));
    }

    private async ValueTask<PersistedCommerceState> ReadStateAsync()
    {
        var json = await _database.StringGetAsync(_stateKey).ConfigureAwait(false);
        return json.IsNullOrEmpty
            ? new PersistedCommerceState()
            : JsonSerializer.Deserialize<PersistedCommerceState>((string)json!, JsonOptions)
              ?? new PersistedCommerceState();
    }

    private ValueTask WriteStateAsync(PersistedCommerceState state)
    {
        return new ValueTask(_database.StringSetAsync(_stateKey, JsonSerializer.Serialize(state, JsonOptions)));
    }

    private async ValueTask<T> WithStateAsync<T>(
        Func<PersistedCommerceState, T> action,
        CancellationToken cancellationToken)
    {
        var token = await AcquireLockAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var state = await ReadStateAsync().ConfigureAwait(false);
            var result = action(state);
            await WriteStateAsync(state).ConfigureAwait(false);
            return result;
        }
        finally
        {
            await ReleaseLockAsync(token).ConfigureAwait(false);
        }
    }

    private async ValueTask WithStateAsync(
        Action<PersistedCommerceState> action,
        CancellationToken cancellationToken)
    {
        await WithStateAsync(state =>
        {
            action(state);
            return true;
        }, cancellationToken).ConfigureAwait(false);
    }
}

internal sealed class PersistedCommerceState
{
    public long NextOrderSequence { get; set; }

    public Dictionary<string, CartSeed> Carts { get; set; } = new(StringComparer.Ordinal);

    public Dictionary<string, int> Inventory { get; set; } = new(StringComparer.Ordinal);

    public Dictionary<string, PaymentMethodSeed> PaymentMethods { get; set; } = new(StringComparer.Ordinal);

    public HashSet<string> ShippingAddresses { get; set; } = new(StringComparer.Ordinal);

    public Dictionary<string, IdempotencyMapping> Idempotency { get; set; } = new(StringComparer.Ordinal);

    public Dictionary<string, List<StoredOrderEvent>> Events { get; set; } = new(StringComparer.Ordinal);

    public Dictionary<string, OrderProjectionState> ReadModels { get; set; } = new(StringComparer.Ordinal);

    public Dictionary<string, InventoryReservation> Reservations { get; set; } = new(StringComparer.Ordinal);

    public Dictionary<string, string> ReleasedReservations { get; set; } = new(StringComparer.Ordinal);

    public Dictionary<string, string> PaymentAttempts { get; set; } = new(StringComparer.Ordinal);

    public Dictionary<string, string> Payments { get; set; } = new(StringComparer.Ordinal);

    public Dictionary<string, string> OrderPaymentMethods { get; set; } = new(StringComparer.Ordinal);
}

internal sealed record InventoryReservation(
    string OrderId,
    InventoryReservationLine[] Lines);

internal sealed record InventoryReservationLine(
    string Sku,
    int Quantity);
