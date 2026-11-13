import json, pathlib, subprocess
root = pathlib.Path('/root/worktree/zlink-3867e/framework/languages/cpp')
job = pathlib.Path('/root/jobs/fx/cx/v21/s033/cpr2')
fixture = root / 'tests/fixtures/request-wire-main-213e579f52.json'
original = fixture.read_bytes()
try:
    data = json.loads(original)
    value = data['cases'][0]['application_payload_hex']
    data['cases'][0]['application_payload_hex'] = value[:-2] + f'{int(value[-2:],16)^1:02x}'
    fixture.write_text(json.dumps(data, indent=2) + '\n')
    with (job / 'fixture-corruption.log').open('w') as output:
        result = subprocess.run([str(root/'build/linux-ninja-release/test_cpp_framework_request_wire_identity'), '--gtest_filter=MainEncoderFixtures/RequestWireIdentity.PreparedServiceAndRouteMatchMainEncoderBytes/0', '--gtest_color=no'], stdout=output, stderr=subprocess.STDOUT)
finally:
    fixture.write_bytes(original)
if result.returncode != 1:
    raise SystemExit(f'Expected rejection exit=1, got {result.returncode}')
output = (job / 'fixture-corruption.log').read_text()
assert '[ RUN      ] MainEncoderFixtures/RequestWireIdentity.PreparedServiceAndRouteMatchMainEncoderBytes/0' in output
assert '[  FAILED  ] 1 test' in output
assert fixture.read_bytes() == original
print('One named fixture-comparison test ran and rejected the corrupted fixture with exit=1; original bytes restored.')
