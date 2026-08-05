package systems.zlink.samples.supportchat.server.configuration;

public final class SampleNames {
    public static final String ApiChannel = "supportchat-api";
    public static final String SupportChannel = "supportchat-support";
    public static final String SupportActorMesh = "supportchat-actors";
    public static final String SupportActorType = "support-user";
    public static final String ConversationSpotType = "supportchat.conversation";
    public static final String StreamNode = "supportchat-session";
    public static final int AgentCapacity = 3;
    public static final String ConversationIdMetadataKey = "ConversationId";
    public static final String ServerEvidenceMarker = "supportchat-server-evidence=completed";
    public static final String ClientMarker = "supportchat=completed";

    private SampleNames() {
    }

    public static final class Roles {
        public static final String Customer = "Customer";
        public static final String Agent = "Agent";

        private Roles() {
        }
    }

    public static final class Statuses {
        public static final String WaitingForAgent = "WaitingForAgent";
        public static final String Active = "Active";
        public static final String WaitingForClose = "WaitingForClose";
        public static final String Closed = "Closed";

        private Statuses() {
        }
    }
}
