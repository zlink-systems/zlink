import { useEffect } from 'preact/hooks';

export function useKeyboardMovement(move: (dx: number, dy: number) => void, enabled: boolean): void {
  useEffect(() => {
    if (!enabled) return undefined;
    const onKey = (event: KeyboardEvent) => {
      const vectors: Record<string, readonly [number, number]> = {
        ArrowLeft: [-5, 0], ArrowRight: [5, 0], ArrowUp: [0, -5], ArrowDown: [0, 5],
      };
      const vector = vectors[event.key];
      if (vector === undefined) return;
      event.preventDefault();
      move(...vector);
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [enabled, move]);
}
