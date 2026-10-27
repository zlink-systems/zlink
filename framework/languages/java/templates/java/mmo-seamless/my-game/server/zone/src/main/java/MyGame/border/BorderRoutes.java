package MyGame.border;

import MyGame.BorderPresence;
import MyGame.Names;
import MyGame.Rules;
import MyGame.zone.Zone;

import systems.zlink.framework.handlers.ZLinkSpotSubscription;
import systems.zlink.framework.spots.*;

import java.util.Map;
import java.util.concurrent.CompletionStage;

public final class BorderRoutes {
    private BorderRoutes() {}

    public static final String TOPIC_1_2 = "zone-1:zone-2";
    public static final String TOPIC_2_1 = "zone-2:zone-1";
    public static final String TOPIC_2_3 = "zone-2:zone-3";
    public static final String TOPIC_3_2 = "zone-3:zone-2";
    public static final String TOPIC_3_4 = "zone-3:zone-4";
    public static final String TOPIC_4_3 = "zone-4:zone-3";
    public static final String TOPIC_4_5 = "zone-4:zone-5";
    public static final String TOPIC_5_4 = "zone-5:zone-4";
    public static final String TOPIC_5_6 = "zone-5:zone-6";
    public static final String TOPIC_6_5 = "zone-6:zone-5";
    private static final Map<String, Class<?>> ROUTES =
            Map.ofEntries(
                    Map.entry(TOPIC_1_2, Route1To2.class),
                    Map.entry(TOPIC_2_1, Route2To1.class),
                    Map.entry(TOPIC_2_3, Route2To3.class),
                    Map.entry(TOPIC_3_2, Route3To2.class),
                    Map.entry(TOPIC_3_4, Route3To4.class),
                    Map.entry(TOPIC_4_3, Route4To3.class),
                    Map.entry(TOPIC_4_5, Route4To5.class),
                    Map.entry(TOPIC_5_4, Route5To4.class),
                    Map.entry(TOPIC_5_6, Route5To6.class),
                    Map.entry(TOPIC_6_5, Route6To5.class));

    public static void register(ZLinkSpotHandlerRegistry handlers, String source, String target) {
        var type = ROUTES.get(Rules.borderTopic(source, target));
        if (type == null) throw new IllegalArgumentException("Unknown border route");
        handlers.addHandler(type);
    }

    @ZLinkSpotSubscription(channelName = Names.BORDER, topic = TOPIC_1_2)
    public static final class Route1To2
            implements ZLinkSpotSubscriptionHandler<Zone, BorderPresence> {
        public CompletionStage<Void> handle(Zone spot, BorderPresence presence) {
            return spot.showBorder(presence);
        }
    }

    @ZLinkSpotSubscription(channelName = Names.BORDER, topic = TOPIC_2_1)
    public static final class Route2To1
            implements ZLinkSpotSubscriptionHandler<Zone, BorderPresence> {
        public CompletionStage<Void> handle(Zone spot, BorderPresence presence) {
            return spot.showBorder(presence);
        }
    }

    @ZLinkSpotSubscription(channelName = Names.BORDER, topic = TOPIC_2_3)
    public static final class Route2To3
            implements ZLinkSpotSubscriptionHandler<Zone, BorderPresence> {
        public CompletionStage<Void> handle(Zone spot, BorderPresence presence) {
            return spot.showBorder(presence);
        }
    }

    @ZLinkSpotSubscription(channelName = Names.BORDER, topic = TOPIC_3_2)
    public static final class Route3To2
            implements ZLinkSpotSubscriptionHandler<Zone, BorderPresence> {
        public CompletionStage<Void> handle(Zone spot, BorderPresence presence) {
            return spot.showBorder(presence);
        }
    }

    @ZLinkSpotSubscription(channelName = Names.BORDER, topic = TOPIC_3_4)
    public static final class Route3To4
            implements ZLinkSpotSubscriptionHandler<Zone, BorderPresence> {
        public CompletionStage<Void> handle(Zone spot, BorderPresence presence) {
            return spot.showBorder(presence);
        }
    }

    @ZLinkSpotSubscription(channelName = Names.BORDER, topic = TOPIC_4_3)
    public static final class Route4To3
            implements ZLinkSpotSubscriptionHandler<Zone, BorderPresence> {
        public CompletionStage<Void> handle(Zone spot, BorderPresence presence) {
            return spot.showBorder(presence);
        }
    }

    @ZLinkSpotSubscription(channelName = Names.BORDER, topic = TOPIC_4_5)
    public static final class Route4To5
            implements ZLinkSpotSubscriptionHandler<Zone, BorderPresence> {
        public CompletionStage<Void> handle(Zone spot, BorderPresence presence) {
            return spot.showBorder(presence);
        }
    }

    @ZLinkSpotSubscription(channelName = Names.BORDER, topic = TOPIC_5_4)
    public static final class Route5To4
            implements ZLinkSpotSubscriptionHandler<Zone, BorderPresence> {
        public CompletionStage<Void> handle(Zone spot, BorderPresence presence) {
            return spot.showBorder(presence);
        }
    }

    @ZLinkSpotSubscription(channelName = Names.BORDER, topic = TOPIC_5_6)
    public static final class Route5To6
            implements ZLinkSpotSubscriptionHandler<Zone, BorderPresence> {
        public CompletionStage<Void> handle(Zone spot, BorderPresence presence) {
            return spot.showBorder(presence);
        }
    }

    @ZLinkSpotSubscription(channelName = Names.BORDER, topic = TOPIC_6_5)
    public static final class Route6To5
            implements ZLinkSpotSubscriptionHandler<Zone, BorderPresence> {
        public CompletionStage<Void> handle(Zone spot, BorderPresence presence) {
            return spot.showBorder(presence);
        }
    }
}
