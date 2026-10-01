/** Formats absent diagnostic text consistently across the Node runtime. */
export function diagnosticTextOrAbsent(value: string | null | undefined): string {
  return value ?? '<none>';
}
