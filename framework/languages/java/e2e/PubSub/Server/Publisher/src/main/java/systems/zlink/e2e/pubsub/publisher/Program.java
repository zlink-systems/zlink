package systems.zlink.e2e.pubsub.publisher;

public final class Program {
    private Program() {
    }

    public static void main(String[] args) {
        new PublisherApplication().run(args);
    }
}
