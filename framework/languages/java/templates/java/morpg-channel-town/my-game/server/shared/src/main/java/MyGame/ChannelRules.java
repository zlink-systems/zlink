package MyGame;

public final class ChannelRules {
    private ChannelRules() {}

    public static String validate(long channel) {
        if (channel > Names.MAXIMUM_INTEGER || channel < -Names.MAXIMUM_INTEGER)
            return Rules.INTEGER_OUT_OF_RANGE;
        if (channel < 1 || channel > Names.CHANNEL_COUNT) return Rules.INVALID_CHANNEL;
        return null;
    }
}
