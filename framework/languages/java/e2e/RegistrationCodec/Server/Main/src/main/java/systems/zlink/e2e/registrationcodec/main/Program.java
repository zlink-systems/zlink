package systems.zlink.e2e.registrationcodec.main;

public final class Program {
    private Program() {
    }

    public static void main(String[] args) {
        new RegistrationCodecServerApplication().run(args);
    }
}
