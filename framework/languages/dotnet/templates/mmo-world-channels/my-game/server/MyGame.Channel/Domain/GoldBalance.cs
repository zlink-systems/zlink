namespace MyGame.Channel.Domain;

public sealed class GoldBalance(long value)
{
    public long Value { get; private set; } = value >= 0 ? value : throw new ArgumentOutOfRangeException(nameof(value));
    public long AfterEarning(long amount)
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(amount);
        return checked(Value + amount);
    }
    public void Restore(long value)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(value);
        Value = value;
    }
}
