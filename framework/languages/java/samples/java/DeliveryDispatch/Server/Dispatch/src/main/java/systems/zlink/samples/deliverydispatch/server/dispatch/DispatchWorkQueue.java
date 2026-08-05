package systems.zlink.samples.deliverydispatch.server.dispatch;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.CompletionStage;
import systems.zlink.samples.deliverydispatch.shared.contracts.Messages;

public final class DispatchWorkQueue implements AutoCloseable {
    private final DispatchWorker worker;
    private final ExecutorService executor = Executors.newSingleThreadExecutor();

    public DispatchWorkQueue(DispatchWorker worker) {
        this.worker = worker;
    }

    public void enqueue(Messages.CreateDeliveryReq request) {
        executor.submit(() -> {
            try {
                System.out.println("deliverydispatch-dispatch-start=" + request.deliveryId());
                worker.dispatch(new Messages.AssignDeliveryMsg(
                    request.deliveryId(),
                    request.customerId(),
                    request.pickupAddress(),
                    request.dropoffAddress())).whenComplete((ignored, error) -> {
                        if (error == null) {
                            System.out.println("deliverydispatch-dispatch-finished=" + request.deliveryId());
                        } else {
                            System.err.println("deliverydispatch-dispatch-failed=" + request.deliveryId()
                                + ": " + error.getMessage());
                            error.printStackTrace(System.err);
                        }
                    });
            } catch (RuntimeException ex) {
                System.err.println("deliverydispatch-dispatch-failed=" + request.deliveryId() + ": "
                    + ex.getMessage());
                ex.printStackTrace(System.err);
            }
        });
    }

    public CompletionStage<Messages.ServerAssertionResponse> assertServerEvidence(
        Messages.ServerAssertionRequest request) {
        return worker.assertServerEvidence(request);
    }

    @Override
    public void close() {
        executor.shutdownNow();
    }
}
