package MyGame.border;

import MyGame.BorderPresence;
import MyGame.Names;
import MyGame.Rules;
import MyGame.field.Field;

import systems.zlink.framework.handlers.ZLinkSpotSubscription;
import systems.zlink.framework.spots.*;

import java.util.Map;
import java.util.concurrent.CompletionStage;

public final class BorderRoutes {
    private BorderRoutes() {}

    public static final String TOPIC_1_2 = "field-1:field-2";
    public static final String TOPIC_2_1 = "field-2:field-1";
    public static final String TOPIC_2_3 = "field-2:field-3";
    public static final String TOPIC_3_2 = "field-3:field-2";
    public static final String TOPIC_3_4 = "field-3:field-4";
    public static final String TOPIC_4_3 = "field-4:field-3";
    private static final Map<String, Class<?>> ROUTES =
            Map.ofEntries(
                    Map.entry(TOPIC_1_2, Route1To2.class),
                    Map.entry(TOPIC_2_1, Route2To1.class),
                    Map.entry(TOPIC_2_3, Route2To3.class),
                    Map.entry(TOPIC_3_2, Route3To2.class),
                    Map.entry(TOPIC_3_4, Route3To4.class),
                    Map.entry(TOPIC_4_3, Route4To3.class));

    public static void register(ZLinkSpotHandlerRegistry handlers, String source, String target) {
        var type = ROUTES.get(Rules.borderTopic(source, target));
        if (type == null) throw new IllegalArgumentException("Unknown border route");
        handlers.addHandler(type);
    }

    @ZLinkSpotSubscription(channelName = Names.BORDER, topic = TOPIC_1_2)
    public static final class Route1To2
            implements ZLinkSpotSubscriptionHandler<Field, BorderPresence> {
        public CompletionStage<Void> handle(Field spot, BorderPresence presence) {
            return spot.showBorder(presence);
        }
    }

    @ZLinkSpotSubscription(channelName = Names.BORDER, topic = TOPIC_2_1)
    public static final class Route2To1
            implements ZLinkSpotSubscriptionHandler<Field, BorderPresence> {
        public CompletionStage<Void> handle(Field spot, BorderPresence presence) {
            return spot.showBorder(presence);
        }
    }

    @ZLinkSpotSubscription(channelName = Names.BORDER, topic = TOPIC_2_3)
    public static final class Route2To3
            implements ZLinkSpotSubscriptionHandler<Field, BorderPresence> {
        public CompletionStage<Void> handle(Field spot, BorderPresence presence) {
            return spot.showBorder(presence);
        }
    }

    @ZLinkSpotSubscription(channelName = Names.BORDER, topic = TOPIC_3_2)
    public static final class Route3To2
            implements ZLinkSpotSubscriptionHandler<Field, BorderPresence> {
        public CompletionStage<Void> handle(Field spot, BorderPresence presence) {
            return spot.showBorder(presence);
        }
    }

    @ZLinkSpotSubscription(channelName = Names.BORDER, topic = TOPIC_3_4)
    public static final class Route3To4
            implements ZLinkSpotSubscriptionHandler<Field, BorderPresence> {
        public CompletionStage<Void> handle(Field spot, BorderPresence presence) {
            return spot.showBorder(presence);
        }
    }

    @ZLinkSpotSubscription(channelName = Names.BORDER, topic = TOPIC_4_3)
    public static final class Route4To3
            implements ZLinkSpotSubscriptionHandler<Field, BorderPresence> {
        public CompletionStage<Void> handle(Field spot, BorderPresence presence) {
            return spot.showBorder(presence);
        }
    }
}
