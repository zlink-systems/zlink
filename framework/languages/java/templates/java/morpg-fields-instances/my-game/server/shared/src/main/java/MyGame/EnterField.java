package MyGame;

public record EnterField(double x, long sequence, int remainingMs) {
    public EnterField(double x, long sequence) { this(x, sequence, 0); }
}
