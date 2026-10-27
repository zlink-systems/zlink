package MyGame.border;

import MyGame.BorderPresence;
import MyGame.Names;
import MyGame.Rules;
import MyGame.field.Field;
import systems.zlink.framework.spots.*;
import java.util.concurrent.CompletionStage;

public final class BorderRoutes {
    private BorderRoutes() {}

    public static void register(ZLinkSpotHandlerRegistry handlers, String source, String target) {
        handlers.addSubscribe(BorderHandler.class, Names.BORDER, Rules.borderTopic(source, target));
    }

    public static final class BorderHandler implements ZLinkSpotSubscriptionHandler<Field, BorderPresence> {
        public CompletionStage<Void> handle(Field spot, BorderPresence presence) {
            return spot.showBorder(presence);
        }
    }
}
