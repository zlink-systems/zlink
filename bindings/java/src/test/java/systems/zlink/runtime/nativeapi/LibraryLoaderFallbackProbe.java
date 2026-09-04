package systems.zlink.runtime.nativeapi;

public final class LibraryLoaderFallbackProbe {
    private LibraryLoaderFallbackProbe() {}

    public static void main(String[] args) {
        LibraryLoader.lookup();
    }
}
