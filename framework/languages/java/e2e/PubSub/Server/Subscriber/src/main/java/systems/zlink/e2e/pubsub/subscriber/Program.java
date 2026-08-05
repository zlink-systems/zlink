package systems.zlink.e2e.pubsub.subscriber;

public final class Program {
    private Program() {
    }

    public static void main(String[] args) {
        new SubscriberApplication().run(args);
    }
}
