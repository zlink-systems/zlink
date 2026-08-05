class TicTacToeBoard {
  private readonly cells: string[];

  constructor() {
    this.cells = Array(9).fill('.');
  }

  place(mark: string, cell: number): void {
    if (!Number.isInteger(cell) || cell < 0 || cell > 8) {
      throw new Error(`Cell '${cell}' is outside the board.`);
    }
    if (this.cells[cell] !== '.') {
      throw new Error(`Cell '${cell}' is already occupied.`);
    }
    this.cells[cell] = mark;
  }

  hasWon(mark: string): boolean {
    for (const [a, b, c] of winningLines()) {
      if (this.cells[a] === mark && this.cells[b] === mark && this.cells[c] === mark) {
        return true;
      }
    }
    return false;
  }

  isFull(): boolean {
    return this.cells.every((cell) => cell !== '.');
  }

  snapshot(): string {
    return this.cells.join('');
  }
}

function winningLines(): readonly (readonly [number, number, number])[] {
  return [
    [0, 1, 2],
    [3, 4, 5],
    [6, 7, 8],
    [0, 3, 6],
    [1, 4, 7],
    [2, 5, 8],
    [0, 4, 8],
    [2, 4, 6]
  ];
}

export { TicTacToeBoard };
