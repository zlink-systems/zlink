class BingoCard {
  readonly numbers: number[];
  readonly marks: boolean[];

  constructor(numbers: number[]) {
    if (numbers.length !== 9) {
      throw new Error('Bingo card must contain exactly 9 cells.');
    }
    const values = numbers.filter((number) => number !== 0);
    if (values.some((number) => !Number.isInteger(number) || number < 1 || number > 15)) {
      throw new Error('Bingo card numbers must be integers from 1 to 15.');
    }
    if (new Set(values).size !== values.length) {
      throw new Error('Bingo card numbers must be unique.');
    }
    if (numbers[4] !== 0) {
      throw new Error('Bingo card center cell must be the free cell.');
    }
    this.numbers = numbers;
    this.marks = numbers.map((number) => number === 0);
  }

  mark(number: number): number {
    this.numbers.forEach((value, index) => {
      if (value === number) {
        this.marks[index] = true;
      }
    });
    return this.completedLines();
  }

  completedLines(): number {
    const lines = [
      [0, 1, 2],
      [3, 4, 5],
      [6, 7, 8],
      [0, 3, 6],
      [1, 4, 7],
      [2, 5, 8],
      [0, 4, 8],
      [2, 4, 6]
    ];
    return lines.filter((line) => line.every((index) => this.marks[index])).length;
  }
}

export { BingoCard };
