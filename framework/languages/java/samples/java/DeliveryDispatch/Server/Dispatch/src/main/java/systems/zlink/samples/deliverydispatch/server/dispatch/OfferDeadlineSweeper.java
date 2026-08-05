package systems.zlink.samples.deliverydispatch.server.dispatch;

import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import systems.zlink.samples.deliverydispatch.server.configuration.SampleTimings;
import systems.zlink.samples.deliverydispatch.server.dispatch.DeliveryOfferStore.DeliveryOffer;

/**
 * The offer deadline. It is a timer, not a wait: nothing is blocked on a courier, so a lapsed offer
 * only moves on because someone comes looking for it (common sample spec section 7.4).
 */
public final class OfferDeadlineSweeper implements AutoCloseable {
    private final DeliveryOfferStore offers;
    private final DispatchWorker worker;
    private final ScheduledExecutorService scheduler =
        Executors.newSingleThreadScheduledExecutor(runnable -> {
            Thread thread = new Thread(runnable, "deliverydispatch-offer-sweeper");
            thread.setDaemon(true);
            return thread;
        });

    public OfferDeadlineSweeper(DeliveryOfferStore offers, DispatchWorker worker) {
        this.offers = offers;
        this.worker = worker;
        long period = SampleTimings.OfferSweepInterval.toMillis();
        scheduler.scheduleAtFixedRate(this::sweep, period, period, TimeUnit.MILLISECONDS);
    }

    @Override
    public void close() {
        scheduler.shutdownNow();
    }

    private void sweep() {
        for (DeliveryOffer offer : offers.takeExpired()) {
            System.out.println("deliverydispatch dispatch: offer expired delivery="
                + offer.request().deliveryId() + " attempt=" + offer.attempt());
            try {
                worker.reassign(offer);
            } catch (RuntimeException error) {
                // The sweeper comes back next tick; one failed reassign must not stop it.
                System.err.println("deliverydispatch dispatch: reassign failed delivery="
                    + offer.request().deliveryId() + ": " + error.getMessage());
            }
        }
    }
}
