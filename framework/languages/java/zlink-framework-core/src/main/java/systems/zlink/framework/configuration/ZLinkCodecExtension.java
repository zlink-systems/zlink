package systems.zlink.framework.configuration;

/**
 * Registers a payload codec with the framework codec registry. Built-in codecs
 * and user-defined codecs use the same contract so codec selection changes do
 * not change handler or client payload APIs.
 */
@FunctionalInterface
public interface ZLinkCodecExtension {
    void register(ZLinkCodecRegistrar codecs);
}
