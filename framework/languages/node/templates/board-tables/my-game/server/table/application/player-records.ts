export abstract class PlayerRecords {
  abstract loadPlayer(playerId: string): Promise<bigint>;
  abstract commitRound(
    tableId: string,
    handNumber: bigint,
    results: readonly { playerId: string; chips: bigint }[]
  ): Promise<void>;
}
