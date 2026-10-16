package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

import java.io.IOException;
import java.lang.classfile.ClassFile;
import java.lang.classfile.CodeModel;
import java.lang.classfile.Instruction;
import java.lang.classfile.Opcode;
import java.util.ArrayList;
import java.util.List;

final class ZLinkServiceCompletionBudgetTest {
    @Test
    void completionPublicationAndDrainAcquireNoMonitors() throws IOException {
        String resource = ZLinkServiceCompletionDispatcher.class.getSimpleName() + ".class";
        List<String> acquisitions = new ArrayList<>();
        try (var input = ZLinkServiceCompletionDispatcher.class.getResourceAsStream(resource)) {
            var model = ClassFile.of().parse(input.readAllBytes());
            for (var method : model.methods()) {
                if (method.code().isEmpty()) continue;
                CodeModel code = method.code().orElseThrow();
                for (var element : code) {
                    if (element instanceof Instruction instruction
                            && instruction.opcode() == Opcode.MONITORENTER) {
                        acquisitions.add(method.methodName().stringValue());
                    }
                }
            }
        }
        assertEquals(List.of(), acquisitions, "B3 completion monitor budget");
    }
}
