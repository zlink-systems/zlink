from pathlib import Path
import re
root=Path('framework/languages/cpp/tests/Zlink.Framework.UnitTests')
for p in root.glob('*.cpp'):
 s=p.read_text()
 s=re.sub(r'zlink::framework::test::claim_mailbox \((\w+(?:->|\.)mailbox \(\)),\s*((?:runtime::)?mesh::service_mailbox_domain_t::infrastructure)',r'\1.try_claim (\2',s)
 if 'claim_mailbox (' not in s:
  s=s.replace('#include "mailbox_claim_fixture.hpp"\n','')
 if s!=p.read_text(): p.write_text(s)
# Only explicit application claim handles change their type.
for name in ['test_cpp_framework_route_mesh_weight_update.cpp','test_cpp_framework_relocation_seal_contract.cpp']:
 p=root/name;s=p.read_text();s=re.sub(r'std::shared_ptr<mesh::service_mailbox_claim_t> (claim|unexpected);',r'zlink::framework::test::mailbox_claim_t \1;',s);p.write_text(s)
p=root/'test_cpp_framework_mesh_metrics.cpp';s=p.read_text().replace('std::shared_ptr<mesh::service_mailbox_claim_t> claim_request','zlink::framework::test::mailbox_claim_t claim_request');s=s.replace('    return nullptr;\n}', '    return {};\n}');p.write_text(s)
p=root/'test_cpp_framework_m6a_runtime.cpp';s=p.read_text().replace('const mesh::service_mailbox_claim_t &claim) {\n        for (const auto &record : claim.records)', 'const zlink::framework::test::mailbox_claim_t &claim) {\n        for (const auto &record : claim.records)').replace('verify_retained_payloads (*claim);','verify_retained_payloads (claim);').replace('verify_retained_payloads (*quiesce_claim);','verify_retained_payloads (quiesce_claim);');a=s.index('void verify_',s.index('4368') if False else 0) if False else 0
# Identify declarations whose first following assignment uses the application consumer.
for m in list(re.finditer(r'std::shared_ptr<mesh::service_mailbox_claim_t> claim;',s))[::-1]:
 following=s[m.end():m.end()+750]
 if 'claim_mailbox (' in following and '::application' in following.split('claim_mailbox (',1)[1][:200]:
  s=s[:m.start()]+'zlink::framework::test::mailbox_claim_t claim;'+s[m.end():]
p.write_text(s)
