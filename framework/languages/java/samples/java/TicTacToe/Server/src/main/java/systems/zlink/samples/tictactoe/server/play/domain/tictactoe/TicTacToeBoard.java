package systems.zlink.samples.tictactoe.server.play.domain.tictactoe;

public final class TicTacToeBoard {
    private static final int[][] WinningLines = {
        {0, 1, 2}, {3, 4, 5}, {6, 7, 8},
        {0, 3, 6}, {1, 4, 7}, {2, 5, 8},
        {0, 4, 8}, {2, 4, 6}
    };

    private final char[] cells = ".........".toCharArray();

    public void place(int cell, String mark) {
        if (cell < 0 || cell >= cells.length || cells[cell] != '.') {
            throw new IllegalArgumentException("invalid cell");
        }
        cells[cell] = mark.charAt(0);
    }

    public boolean hasWon(String mark) {
        char value = mark.charAt(0);
        for (int[] line : WinningLines) {
            if (cells[line[0]] == value && cells[line[1]] == value && cells[line[2]] == value) {
                return true;
            }
        }
        return false;
    }

    public boolean isFull() {
        return new String(cells).indexOf('.') < 0;
    }

    public String snapshot() {
        return new String(cells);
    }
}
