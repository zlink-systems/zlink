# .NET Framework Guidelines

- `bindings/dotnet/`은 public API로만 사용한다.
- `System.Reflection`, `NonPublic`, `MethodInfo.Invoke`, private field access와 임의의
  `InternalsVisibleTo`로 binding 내부를 우회하지 않는다.
- 필요한 binding 기능이 public API에 없으면 reflection adapter를 만들지 않는다. 계약 근거를
  확인한 뒤 binding public API로 추가하고 Framework에서 직접 호출한다.
