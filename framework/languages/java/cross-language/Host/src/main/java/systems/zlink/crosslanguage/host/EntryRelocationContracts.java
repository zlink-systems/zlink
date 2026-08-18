package systems.zlink.crosslanguage.host;

/**
 * DTOs for the cross-language entry-spot relocation scenario (spec
 * 28-relocation-flow.ko.md:589-591): an actor is created via the entry spot
 * (the JoinEntrySpot admission-free path, spec 15-spot-actor.ko.md:489), a
 * whole-node relocate() drain moves it to the peer node, and a post-
 * relocation probe confirms it answers on the new owner.
 */
public final class EntryRelocationContracts {
    private EntryRelocationContracts() {
    }

    public record CrossLangActorCreateReq(int stateVersion, int applicationStateBytes) {
    }

    public record CrossLangProbeReq(String marker) {
    }

    public record CrossLangProbeRes(
        String nodeRid, int stateVersion, int applicationStateBytes, String marker) {
    }
}
