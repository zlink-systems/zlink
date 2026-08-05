package systems.zlink.samples.tictactoe.client;

public final class Program {
    private Program() {
    }

    public static void main(String[] args) throws Exception {
        TicTacToeClientOptions clientOptions = TicTacToeClientArguments.parse(args);
        new TicTacToeClientScenario().run(clientOptions);
        System.out.println("tictactoe=completed");
    }
}
