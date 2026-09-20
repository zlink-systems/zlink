package systems.zlink.samples.deliverydispatch.server.dispatch;

import systems.zlink.samples.deliverydispatch.shared.contracts.Messages;

import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class DispatchWorkQueue implements AutoCloseable {
    private final DispatchWorker worker;
    private final ExecutorService executor = Executors.newSingleThreadExecutor();

    public DispatchWorkQueue(DispatchWorker worker) {
        this.worker = worker;
    }

    public void enqueue(Messages.AssignDeliveryMsg request) {
        executor.submit(
                () -> {
                    try {
                        System.out.println(
                                "deliverydispatch-dispatch-start=" + request.deliveryId());
                        worker.dispatch(request)
                                .whenComplete(
                                        (ignored, error) -> {
                                            if (error == null) {
                                                System.out.println(
                                                        "deliverydispatch-dispatch-finished="
                                                                + request.deliveryId());
                                            } else {
                                                System.err.println(
                                                        "deliverydispatch-dispatch-failed="
                                                                + request.deliveryId()
                                                                + ": "
                                                                + error.getMessage());
                                                error.printStackTrace(System.err);
                                            }
                                        });
                    } catch (RuntimeException ex) {
                        System.err.println(
                                "deliverydispatch-dispatch-failed="
                                        + request.deliveryId()
                                        + ": "
                                        + ex.getMessage());
                        ex.printStackTrace(System.err);
                    }
                });
    }

    public CompletionStage<Messages.ServerAssertionRes> assertServerEvidence(
            Messages.ServerAssertionReq request) {
        return worker.assertServerEvidence(request);
    }

    @Override
    public void close() {
        executor.shutdownNow();
    }
}
