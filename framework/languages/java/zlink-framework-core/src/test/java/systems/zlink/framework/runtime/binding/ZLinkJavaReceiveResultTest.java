package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;

import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.sockets.RecvResult;

final class ZLinkJavaReceiveResultTest {
    @ParameterizedTest
    @EnumSource(value = RecvResult.class, mode = EnumSource.Mode.EXCLUDE, names = "OK")
    void onlyNoDataMayBecomeAnEmptyReceive(RecvResult result) {
        ZlinkRecvException failure = new ZlinkRecvException(result);
        if (result == RecvResult.NO_DATA) {
            assertFalse(
                    ZLinkJavaSocketSupport.recvOrNoData(
                            () -> {
                                throw failure;
                            }));
        } else {
            assertSame(
                    failure,
                    assertThrows(
                            ZlinkRecvException.class,
                            () ->
                                    ZLinkJavaSocketSupport.recvOrNoData(
                                            () -> {
                                                throw failure;
                                            })));
        }
    }
}
