package systems.zlink.framework.runtime.internal.locations;

/** Internal descriptor write intent shared by the location repositories. */
public interface ZLinkDescriptorWriteIntent {
    boolean isNewClaim();

    boolean isFixedRidNewClaim();

    enum FixedRidNewClaim implements ZLinkDescriptorWriteIntent {
        FIXED_RID_NEW_CLAIM;

        @Override
        public boolean isNewClaim() {
            return true;
        }

        @Override
        public boolean isFixedRidNewClaim() {
            return true;
        }
    }
}
