from pathlib import Path
import re
root=Path('framework/languages/cpp/tests/Zlink.Framework.UnitTests')
for p in root.glob('*.cpp'):
    if p.name in ['test_cpp_framework_claim_dispatch_cost.cpp','test_cpp_framework_receive_role_handoff.cpp']:
        continue
    s=p.read_text()
    # Transport fixtures consume originals directly; no application dispatcher owns them.
    pat=r'(\b\w+(?:->|\.)mailbox \(\))\.try_claim \('
    s2=re.sub(pat,r'zlink::framework::test::claim_mailbox (\1,',s)
    if s!=s2:
        s2=s2.replace('#include ', '#include "mailbox_claim_fixture.hpp"\n#include ',1)
        p.write_text(s2)
