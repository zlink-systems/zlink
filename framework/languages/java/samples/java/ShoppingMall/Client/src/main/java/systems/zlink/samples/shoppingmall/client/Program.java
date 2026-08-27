package systems.zlink.samples.shoppingmall.client;

import java.time.Instant;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.locks.LockSupport;
import systems.zlink.samples.shoppingmall.client.configuration.SampleTimings;
import systems.zlink.samples.shoppingmall.client.configuration.SampleTopology;
import systems.zlink.samples.shoppingmall.server.configuration.SampleNames;
import systems.zlink.samples.shoppingmall.shared.contracts.Messages;
import systems.zlink.httpclient.ZLinkHttpClient;

public final class Program {
    private Program() {
    }

    public static void main(String[] args) throws Exception {
        new ShoppingMallClientScenario(SampleTopology.load(args)).run();
        System.out.println(SampleNames.CompletedMarker);
    }

    private static final class ShoppingMallClientScenario {
        private final SampleTopology topology;

        ShoppingMallClientScenario(SampleTopology topology) {
            this.topology = topology;
        }

        void run() throws Exception {
            Messages.StartOrderRes success = start(
                topology.apiAHttpUrl(),
                "cart-success",
                "pm-ok",
                "start-success");
            ensure(Messages.OrderStatuses.Created.equals(success.status()));
            waitForStatus(success.orderId(), Messages.OrderStatuses.Confirmed);
            emitOrder("success", success);

            Messages.StartOrderRes duplicate = start(
                topology.apiBHttpUrl(),
                "cart-success",
                "pm-ok",
                "start-success");
            ensure(duplicate.orderId().equals(success.orderId()));

            CompletableFuture<Messages.StartOrderRes> firstConcurrent = CompletableFuture.supplyAsync(() ->
                uncheckedStart(topology.apiAHttpUrl(), "cart-success", "pm-ok", "concurrent-order"));
            CompletableFuture<Messages.StartOrderRes> secondConcurrent = CompletableFuture.supplyAsync(() ->
                uncheckedStart(topology.apiBHttpUrl(), "cart-success", "pm-ok", "concurrent-order"));
            Messages.StartOrderRes concurrentA = firstConcurrent.get();
            Messages.StartOrderRes concurrentB = secondConcurrent.get();
            ensure(concurrentA.orderId().equals(concurrentB.orderId()));
            waitForStatus(concurrentA.orderId(), Messages.OrderStatuses.Confirmed);
            emitOrder("concurrent", concurrentA);

            Messages.StartOrderRes inventoryFailure = start(
                topology.apiAHttpUrl(),
                "cart-inventory-fail",
                "pm-ok",
                "inventory-failure");
            waitForStatus(inventoryFailure.orderId(), Messages.OrderStatuses.Failed);
            emitOrder("inventory-failure", inventoryFailure);

            Messages.StartOrderRes paymentFailure = start(
                topology.apiBHttpUrl(),
                "cart-payment-fail",
                "pm-decline",
                "payment-failure");
            waitForStatus(paymentFailure.orderId(), Messages.OrderStatuses.Failed);
            emitOrder("payment-failure", paymentFailure);

            Messages.StartOrderRes scaleOut = start(
                topology.apiBHttpUrl(),
                "cart-success",
                "pm-ok",
                "scale-out-order");
            waitForStatus(scaleOut.orderId(), Messages.OrderStatuses.Confirmed);
            emitOrder("scale-out", scaleOut);
        }

        private Messages.StartOrderRes uncheckedStart(
            String base,
            String cartId,
            String paymentMethodId,
            String idempotencyKey) {
            try {
                return start(base, cartId, paymentMethodId, idempotencyKey);
            } catch (Exception ex) {
                throw new IllegalStateException(ex);
            }
        }

        private Messages.StartOrderRes start(
            String base,
            String cartId,
            String paymentMethodId,
            String idempotencyKey) throws Exception {
            return post(
                base,
                "/orders/start",
                new Messages.StartOrderReq(cartId, "addr-home", paymentMethodId, idempotencyKey),
                Messages.StartOrderRes.class);
        }

        private Messages.OrderState waitForStatus(String orderId, String status) throws Exception {
            Instant deadline = Instant.now().plus(SampleTimings.WorkflowTimeout);
            Messages.OrderState last = null;
            while (Instant.now().isBefore(deadline)) {
                Messages.GetOrderStateRes response = get(
                    topology.apiAHttpUrl(),
                    "/orders/" + orderId,
                    Messages.GetOrderStateRes.class);
                last = response.state();
                if (last != null && status.equals(last.status())) {
                    return last;
                }
                LockSupport.parkNanos(100_000_000L);
            }
            throw new IllegalStateException("Timed out waiting for " + orderId + " to reach " + status
                + ", last=" + last);
        }

        private <T> T get(String base, String path, Class<T> type) throws Exception {
            return ZLinkHttpClient.create(base)
                .get(path)
                .fetch(type)
                .toCompletableFuture()
                .join();
        }

        private <T> T post(String base, String path, Object body, Class<T> type) throws Exception {
            var request = ZLinkHttpClient.create(base).post(path);
            if (!(body instanceof String value) || !value.isEmpty()) {
                request.body(body);
            }
            return request.fetch(type).toCompletableFuture().join();
        }

        private static void ensure(boolean condition) {
            if (!condition) {
                throw new IllegalStateException("Ensure failed");
            }
        }

        private static void emitOrder(String name, Messages.StartOrderRes order) {
            System.out.println("shoppingmall-client-order name=" + name + " order=" + order.orderId());
        }
    }
}
