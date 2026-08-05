package systems.zlink.framework.runtime.internal.handlers;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotSame;
import static org.junit.jupiter.api.Assertions.assertSame;

import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;

final class ZLinkHandlerInstanceOwnerTest {
    @Test
    void reusesHandlerInsideOneActivationAndRecreatesItForTheNextActivation() {
        AtomicInteger creates = new AtomicInteger();
        AtomicInteger destroys = new AtomicInteger();
        ZLinkHandlerActivator activator = new ZLinkHandlerActivator() {
            @Override
            public Object create(Class<?> handlerType) {
                creates.incrementAndGet();
                return new TestHandler();
            }

            @Override
            public void destroy(Object instance) {
                destroys.incrementAndGet();
            }
        };

        TestHandler first;
        try (var activation = new ZLinkHandlerInstanceOwner(activator)) {
            first = (TestHandler) activation.instance(TestHandler.class);
            assertSame(first, activation.instance(TestHandler.class));
        }
        try (var activation = new ZLinkHandlerInstanceOwner(activator)) {
            assertNotSame(first, activation.instance(TestHandler.class));
            activation.close();
        }

        assertEquals(2, creates.get());
        assertEquals(2, destroys.get());
    }

    private static final class TestHandler {
    }
}
