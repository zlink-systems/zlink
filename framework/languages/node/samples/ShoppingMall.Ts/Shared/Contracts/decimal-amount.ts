const DECIMAL_SCALE = 2n;
const DECIMAL_FACTOR = 10n ** DECIMAL_SCALE;

// Below 2^45 major units, every two-decimal value has a unique IEEE-754
// representation that can be recovered after a JSON number parse.
const MAX_DECIMAL_MINOR_UNITS = ((2n ** 45n) * DECIMAL_FACTOR) - 1n;

type DecimalAmountInput = DecimalAmount | number;
declare const decimalWireNumberBrand: unique symbol;
type DecimalWireNumber = number & { readonly [decimalWireNumberBrand]: 'DecimalWireNumber' };

class DecimalAmount {
  private constructor(private readonly minorUnits: bigint) {}

  static fromMinorUnits(minorUnits: bigint): DecimalAmount {
    if (minorUnits < 0n || minorUnits > MAX_DECIMAL_MINOR_UNITS) {
      throw new RangeError('Decimal amount is outside the supported JSON number range.');
    }
    return new DecimalAmount(minorUnits);
  }

  static fromWire(value: unknown): DecimalAmount {
    if (value instanceof DecimalAmount) {
      return value;
    }
    if (typeof value !== 'number' || !Number.isFinite(value) || value < 0) {
      throw new TypeError('Decimal amount must be a non-negative JSON number.');
    }
    const roundedMinorUnits = Math.round(value * Number(DECIMAL_FACTOR));
    if (!Number.isSafeInteger(roundedMinorUnits)
      || roundedMinorUnits / Number(DECIMAL_FACTOR) !== value) {
      throw new RangeError('Decimal amount must have at most two fractional digits.');
    }
    return DecimalAmount.fromMinorUnits(BigInt(roundedMinorUnits));
  }

  toMinorUnits(): bigint {
    return this.minorUnits;
  }

  equalsWire(value: unknown): boolean {
    return DecimalAmount.fromWire(value).minorUnits === this.minorUnits;
  }

  toWireNumber(): DecimalWireNumber {
    return (Number(this.minorUnits) / Number(DECIMAL_FACTOR)) as DecimalWireNumber;
  }
}

export { DecimalAmount, MAX_DECIMAL_MINOR_UNITS };
export type { DecimalAmountInput, DecimalWireNumber };
