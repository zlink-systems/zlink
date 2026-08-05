using ShoppingMall.Client.Configuration;
using ShoppingMall.Shared.Contracts;
using Zlink.HttpClient;

namespace ShoppingMall.Client;

internal sealed class ShoppingMallClientScenario
{
    // End-to-end client story:
    // 1. Start a successful order on API A and wait until the workflow confirms it.
    // 2. Repeat the same idempotency key through API B and verify it returns the same order.
    // 3. Exercise a pending idempotency record that another API instance can resume.
    // 4. Run inventory and payment failure paths and verify the stored failure reasons.
    // 5. Delete and rebuild a read projection, then compare delayed reads across API instances.
    // 6. Start a scale-out order and ask the server to verify all expected workflow evidence.
    public async ValueTask RunAsync(
        ZLinkHttpClient apiA,
        ZLinkHttpClient apiB,
        CancellationToken cancellationToken = default)
    {
        var successReq = new StartOrderReq(
            "cart-success",
            "addr-home",
            "pm-ok",
            "order-success-001");
        var success = await apiA.Post("/orders/start").Body(successReq)
            .Fetch<StartOrderRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(success.State.Status == OrderStatuses.Created, "Assertion failed: success.State.Status == OrderStatuses.Created");
        ZlinkStreamAssert.Ensure(!string.IsNullOrWhiteSpace(success.OrderId), "Assertion failed: !string.IsNullOrWhiteSpace(success.OrderId)");

        var created = await GetOrderAsync(apiA, success.OrderId, cancellationToken);
        ZlinkStreamAssert.Ensure(IsStartedOrConfirmed(created), "Assertion failed: IsStartedOrConfirmed(created)");
        ZlinkStreamAssert.Ensure(created.ShippingAddressId == successReq.ShippingAddressId, "Assertion failed: created.ShippingAddressId == successReq.ShippingAddressId");

        var confirmed = await WaitForStatusAsync(apiA, success.OrderId, OrderStatuses.Confirmed, cancellationToken);
        ZlinkStreamAssert.Ensure(confirmed.ReservationId is not null, "Assertion failed: confirmed.ReservationId is not null");
        ZlinkStreamAssert.Ensure(confirmed.PaymentId is not null, "Assertion failed: confirmed.PaymentId is not null");
        ZlinkStreamAssert.Ensure(confirmed.Amount == 120.00m, "Assertion failed: confirmed.Amount == 120.00m");
        ZlinkStreamAssert.Ensure(confirmed.Currency == "USD", "Assertion failed: confirmed.Currency == \"USD\"");

        var duplicate = await apiB.Post("/orders/start").Body(successReq)
            .Fetch<StartOrderRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(duplicate.OrderId == success.OrderId, "Assertion failed: duplicate.OrderId == success.OrderId");

        var concurrentReq = new StartOrderReq(
            "cart-success",
            "addr-office",
            "pm-ok",
            "order-concurrent-001");
        var concurrentA = apiA.Post("/orders/start").Body(concurrentReq)
            .Fetch<StartOrderRes>(cancellationToken).AsTask();
        var concurrentB = apiB.Post("/orders/start").Body(concurrentReq)
            .Fetch<StartOrderRes>(cancellationToken).AsTask();
        await Task.WhenAll(concurrentA, concurrentB);
        var concurrentAResult = await concurrentA;
        var concurrentBResult = await concurrentB;
        ZlinkStreamAssert.Ensure(concurrentAResult.OrderId == concurrentBResult.OrderId, "Concurrent requests must return the same order.");
        var concurrentConfirmed =
            await WaitForStatusAsync(apiA, concurrentAResult.OrderId, OrderStatuses.Confirmed, cancellationToken);
        ZlinkStreamAssert.Ensure(concurrentConfirmed.Status == OrderStatuses.Confirmed, "Assertion failed: concurrentConfirmed.Status == OrderStatuses.Confirmed");

        var pendingReq = new StartOrderReq(
            "cart-success",
            "addr-office",
            "pm-ok",
            "order-pending-001");
        var pendingHook = await apiA.Post("/self-check/idempotency/pending")
            .Body(new
            {
                pendingReq.IdempotencyKey,
                OrderId = "order-pending-0001"
            })
            .AsyncRaw(cancellationToken);
        ZlinkStreamAssert.Ensure(pendingHook.Status is >= 200 and < 300, "Assertion failed: pendingHook.Status is >= 200 and < 300");
        var pending = await apiB.Post("/orders/start").Body(pendingReq)
            .Fetch<StartOrderRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(pending.OrderId == "order-pending-0001", "Assertion failed: pending.OrderId == \"order-pending-0001\"");
        ZlinkStreamAssert.Ensure(pending.State.Status == OrderStatuses.Created, "Assertion failed: pending.State.Status == OrderStatuses.Created");
        var pendingCreated = await GetOrderAsync(apiA, pending.OrderId, cancellationToken);
        ZlinkStreamAssert.Ensure(IsStartedOrConfirmed(pendingCreated), "Assertion failed: IsStartedOrConfirmed(pendingCreated)");
        ZlinkStreamAssert.Ensure(pendingCreated.ShippingAddressId == pendingReq.ShippingAddressId, "Assertion failed: pendingCreated.ShippingAddressId == pendingReq.ShippingAddressId");

        var resumeReq = new StartOrderReq(
            "cart-success",
            "addr-home",
            "pm-ok",
            "order-resume-001");
        var inventoryReserved = await apiA.Post("/self-check/workflow/inventory-reserved")
            .Body(resumeReq)
            .Fetch<StartOrderRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(inventoryReserved.State.Status == OrderStatuses.InventoryReserved, "Assertion failed: inventoryReserved.State.Status == OrderStatuses.InventoryReserved");
        var resumed = await apiB.Post($"/self-check/workflow/{inventoryReserved.OrderId}/continue")
            .Fetch<ContinueOrderWorkflowRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(resumed.State.Status == OrderStatuses.Confirmed, "Assertion failed: resumed.State.Status == OrderStatuses.Confirmed");
        ZlinkStreamAssert.Ensure(resumed.State.ReservationId == $"reservation-{inventoryReserved.OrderId}", "Assertion failed: resumed.State.ReservationId == $\"reservation-{inventoryReserved.OrderId}\"");
        ZlinkStreamAssert.Ensure(resumed.State.PaymentId == $"payment-{inventoryReserved.OrderId}", "Assertion failed: resumed.State.PaymentId == $\"payment-{inventoryReserved.OrderId}\"");

        var inventoryReq = new StartOrderReq(
            "cart-inventory-fail",
            "addr-home",
            "pm-ok",
            "order-inventory-001");
        var inventoryStarted = await apiA.Post("/orders/start").Body(inventoryReq)
            .Fetch<StartOrderRes>(cancellationToken);
        var inventoryFailed =
            await WaitForStatusAsync(apiA, inventoryStarted.OrderId, OrderStatuses.Failed, cancellationToken);
        ZlinkStreamAssert.Ensure(inventoryFailed.Reason?.Contains("inventory", StringComparison.OrdinalIgnoreCase) == true, "Assertion failed: inventoryFailed.Reason?.Contains(\"inventory\", StringComparison.OrdinalIgnoreCase) == true");

        var paymentReq = new StartOrderReq(
            "cart-payment-fail",
            "addr-home",
            "pm-decline",
            "order-payment-001");
        var paymentStarted = await apiB.Post("/orders/start").Body(paymentReq)
            .Fetch<StartOrderRes>(cancellationToken);
        var paymentFailed =
            await WaitForStatusAsync(apiB, paymentStarted.OrderId, OrderStatuses.Failed, cancellationToken);
        ZlinkStreamAssert.Ensure(paymentFailed.ReservationId is not null, "Assertion failed: paymentFailed.ReservationId is not null");
        ZlinkStreamAssert.Ensure(paymentFailed.Reason?.Contains("payment", StringComparison.OrdinalIgnoreCase) == true, "Assertion failed: paymentFailed.Reason?.Contains(\"payment\", StringComparison.OrdinalIgnoreCase) == true");

        var deleteProjection = await apiA.Post($"/self-check/projection/{success.OrderId}/delete")
            .AsyncRaw(cancellationToken);
        ZlinkStreamAssert.Ensure(deleteProjection.Status is >= 200 and < 300, "Assertion failed: deleteProjection.Status is >= 200 and < 300");
        var healedByContinue = await apiB.Post($"/self-check/workflow/{success.OrderId}/continue")
            .Fetch<ContinueOrderWorkflowRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(healedByContinue.State.Status == OrderStatuses.Confirmed, "Assertion failed: healedByContinue.State.Status == OrderStatuses.Confirmed");

        var deleteProjectionAgain = await apiA.Post($"/self-check/projection/{success.OrderId}/delete")
            .AsyncRaw(cancellationToken);
        ZlinkStreamAssert.Ensure(deleteProjectionAgain.Status is >= 200 and < 300, "Assertion failed: deleteProjectionAgain.Status is >= 200 and < 300");
        var rebuilt = await apiA.Post($"/self-check/projection/{success.OrderId}/rebuild")
            .Fetch<RebuildOrderProjectionRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(rebuilt.State.Status == OrderStatuses.Confirmed, "Assertion failed: rebuilt.State.Status == OrderStatuses.Confirmed");
        var rebuiltRead = await GetOrderAsync(apiB, success.OrderId, cancellationToken);
        ZlinkStreamAssert.Ensure(rebuiltRead.Status == OrderStatuses.Confirmed, "Assertion failed: rebuiltRead.Status == OrderStatuses.Confirmed");

        var delayedFirst = await GetOrderAsync(apiB, paymentStarted.OrderId, cancellationToken);
        var delayedSecond = await GetOrderAsync(apiA, paymentStarted.OrderId, cancellationToken);
        ZlinkStreamAssert.Ensure(delayedFirst.Status == delayedSecond.Status, "Assertion failed: delayedFirst.Status == delayedSecond.Status");
        ZlinkStreamAssert.Ensure(delayedSecond.Status == OrderStatuses.Failed, "Assertion failed: delayedSecond.Status == OrderStatuses.Failed");

        var scaleReq = new StartOrderReq(
            "cart-success",
            "addr-office",
            "pm-ok",
            "order-scale-001");
        var scale = await apiB.Post("/orders/start").Body(scaleReq)
            .Fetch<StartOrderRes>(cancellationToken);
        var scaleConfirmed = await WaitForStatusAsync(apiA, scale.OrderId, OrderStatuses.Confirmed, cancellationToken);
        ZlinkStreamAssert.Ensure(scaleConfirmed.Status == OrderStatuses.Confirmed, "Assertion failed: scaleConfirmed.Status == OrderStatuses.Confirmed");

        var assertion = await apiA.Post("/self-check/assert")
            .Body(new ServerAssertionReq(
                success.OrderId,
                pending.OrderId,
                concurrentAResult.OrderId,
                inventoryReserved.OrderId,
                inventoryStarted.OrderId,
                paymentStarted.OrderId,
                scale.OrderId))
            .Fetch<ServerAssertionRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(assertion.Passed, "Assertion failed: assertion.Passed");
    }

    private static async ValueTask<OrderState> GetOrderAsync(
        ZLinkHttpClient api,
        string orderId,
        CancellationToken cancellationToken)
    {
        var response = await api.Get($"/orders/{orderId}")
            .Fetch<GetOrderStateRes>(cancellationToken);
        return response.State;
    }

    private static async ValueTask<OrderState> WaitForStatusAsync(
        ZLinkHttpClient api,
        string orderId,
        string expectedStatus,
        CancellationToken cancellationToken)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeout.CancelAfter(SampleTimings.WorkflowTimeout);
        while (!timeout.IsCancellationRequested)
        {
            var state = await GetOrderAsync(api, orderId, timeout.Token);
            if (state.Status == expectedStatus) return state;

            await Task.Delay(100, timeout.Token);
        }

        throw new TimeoutException($"Timed out waiting for order '{orderId}' status '{expectedStatus}'.");
    }

    private static bool IsStartedOrConfirmed(OrderState state)
    {
        return state.Status == OrderStatuses.Created
               || state.Status == OrderStatuses.InventoryReserved
               || state.Status == OrderStatuses.PaymentAuthorized
               || state.Status == OrderStatuses.Confirmed;
    }
}

internal sealed record ServerAssertionReq(
    string SuccessfulOrderId,
    string PendingRecoveredOrderId,
    string ConcurrentOrderId,
    string ResumedOrderId,
    string InventoryFailureOrderId,
    string PaymentFailureOrderId,
    string ScaleOutOrderId);

internal sealed record ServerAssertionRes(
    bool Passed,
    string[] Evidence);
