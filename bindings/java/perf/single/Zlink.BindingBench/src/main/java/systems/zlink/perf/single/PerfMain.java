/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.single;



import systems.zlink.perf.PerfUtil;

public final class PerfMain {
    private PerfMain() {
    }

    public static void main(String[] args) {
        PerfUtil.Config config = PerfUtil.parseSingleArgs(args);
        PerfUtil.Result result;
        try {
            result = PerfPatternRegistry.run(config);
        } catch (Throwable failure) {
            result = PerfUtil.classifyFailure(config, failure);
        }
        String line = result.toLine("current");
        if (!line.isEmpty()) {
            System.out.println(line);
        }
        if ("fail".equals(result.status())) {
            System.exit(1);
        }
    }
}
