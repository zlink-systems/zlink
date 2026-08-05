package systems.zlink.framework.runtime.locations;

interface ZLinkAutoConnectExecutor {
    boolean connect(ZLinkAutoConnectPlanner.Target target);

    boolean disconnect(ZLinkAutoConnectPlanner.Target target);

    default void ensureConnected(ZLinkAutoConnectPlanner.Target target) {
    }

    default boolean isManual(ZLinkAutoConnectPlanner.Target target) {
        return false;
    }

    default boolean replace(
        ZLinkAutoConnectPlanner.Target current,
        ZLinkAutoConnectPlanner.Target replacement) {
        return disconnect(current) && connect(replacement);
    }

    default void markNotRequired(ZLinkAutoConnectPlanner.Target target) {
    }

    default void clearNotRequired(ZLinkAutoConnectPlanner.Target target) {
    }

    default void observeAdmissionExpectation(
        ZLinkAutoConnectPlanner.Target target) {
    }

    default void forgetAdmissionExpectation(
        ZLinkAutoConnectPlanner.Target target) {
    }

    ZLinkAutoConnectExecutor NONE = new ZLinkAutoConnectExecutor() {
        @Override
        public boolean connect(ZLinkAutoConnectPlanner.Target target) {
            return true;
        }

        @Override
        public boolean disconnect(ZLinkAutoConnectPlanner.Target target) {
            return true;
        }
    };
}
