import tseslint from 'typescript-eslint';

export default tseslint.config({
  files: ['packages/**/*.ts', 'samples/**/*.ts'],
  ignores: [
    'packages/*/dist/**',
    'samples/**/dist/**'
  ],
  languageOptions: {
    parser: tseslint.parser,
    parserOptions: {
      project: './tsconfig.eslint.json',
      tsconfigRootDir: import.meta.dirname
    }
  },
  plugins: {
    '@typescript-eslint': tseslint.plugin
  },
  rules: {
    '@typescript-eslint/no-unnecessary-condition': 'error',
    '@typescript-eslint/prefer-nullish-coalescing': 'error',
    '@typescript-eslint/strict-boolean-expressions': ['error', {
      allowNullableBoolean: false,
      allowNullableNumber: false,
      allowNullableObject: false,
      allowNullableString: false,
      allowNumber: false,
      allowString: false
    }]
  }
});
