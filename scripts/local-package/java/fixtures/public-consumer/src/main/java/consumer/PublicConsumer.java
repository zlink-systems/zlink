package consumer;

import systems.zlink.contracts.core.Zlink;

final class PublicConsumer {
    public static void main(String[] args) {
        int[] version = Zlink.version();
        if (version.length != 3 || version[0] != @CORE_MAJOR@
                || version[1] != @CORE_MINOR@ || version[2] != @CORE_PATCH@) {
            throw new IllegalStateException(
                "Expected packaged Core @CORE_VERSION@, found "
                    + java.util.Arrays.toString(version));
        }
        System.out.println("ZLINK_CORE_VERSION=@CORE_VERSION@");
    }

    private PublicConsumer() {
    }
}
