from pathlib import Path
r=Path('framework/languages/cpp/tests/Zlink.Framework.UnitTests')
p=r/'test_cpp_framework_receive_role_handoff.cpp';s=p.read_text();a=s.index('// The same observer sees');b=s.index('using original_t =',a);block=s[a:b];core=block[:block.index('void same_owner_negative_controls')]
core=core.replace('bool same_owner_drain_observation','inline bool same_owner_drain_observation')
core=core.replace('    std::atomic_bool single{true};','    std::atomic_bool single{true}, operations_ok{true};')
core=core.replace('EXPECT_TRUE (executor.try_submit_internal (enter));','operations_ok = executor.try_submit_internal (enter) && operations_ok;')
core=core.replace('EXPECT_TRUE (original->transferred ());','operations_ok = original->transferred () && operations_ok;')
core=core.replace('EXPECT_TRUE (executor.try_submit_internal ([&, key] { drain (key); }));','operations_ok = executor.try_submit_internal ([&, key] { drain (key); }) && operations_ok;')
core=core.replace('EXPECT_TRUE (mailbox.try_enqueue (record (owner, i)));','operations_ok = mailbox.try_enqueue (control_record (owner, i)) && operations_ok;')
core=core.replace('return delivered == count && single','return operations_ok && delivered == count && single')
controls=block[block.index('void same_owner_negative_controls'):]
controls=controls.replace('void same_owner_negative_controls (const std::string &owner = "node")','inline bool same_owner_claim_controls (const std::string &owner = "node")').replace('    EXPECT_TRUE (same_owner_drain_observation (owner, publication_fault_t::none));','    bool passed = same_owner_drain_observation (owner, publication_fault_t::none);').replace('        EXPECT_TRUE (detected) << static_cast<int> (fault);','        passed = detected && passed;')
controls=controls.rstrip();controls=controls[:-1]+'    return passed;\n}\n'
header='''/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/mesh/service_mailbox.hpp"
#include <array>
#include <atomic>
#include <iostream>

namespace zlink::framework::test
{
namespace rt = runtime;
namespace mesh = runtime::mesh;
inline mesh::service_mailbox_record_t control_record (std::string owner, unsigned sequence)
{
    return mesh::service_mailbox_record_t::from_parts (
      std::move (owner), mesh::service_mailbox_domain_t::application,
      {zlink::message_t::from (std::to_string (sequence))});
}

'''+core+controls+'}\n'
(r/'same_owner_claim_controls.hpp').write_text(header)
s=s[:a]+'''void same_owner_negative_controls (const std::string &owner = "node")
{
    EXPECT_TRUE (zlink::framework::test::same_owner_claim_controls (owner));
}

'''+s[b:];s=s.replace('#include <gtest/gtest.h>','#include <gtest/gtest.h>\n#include "same_owner_claim_controls.hpp"',1);p.write_text(s)
p=r/'test_cpp_framework_client_server_runtime.cpp';s=p.read_text().replace('#include "mailbox_claim_fixture.hpp"','#include "mailbox_claim_fixture.hpp"\n#include "same_owner_claim_controls.hpp"',1)
s=s.replace('''        verify_gated_owner_control ();
        return 0;''','''        verify_gated_owner_control ();
        assert (zlink::framework::test::same_owner_claim_controls ());
        return 0;''',1)
s=s.replace('''        assert (verify_same_owner_blocking_public_api_repro ());
        return 0;''','''        const bool independent = verify_same_owner_blocking_public_api_repro ();
        const bool controls = zlink::framework::test::same_owner_claim_controls ();
        assert (independent && controls);
        return 0;''',1)
s=s.replace('''        verify_gated_owner_control ();
        assert (independent);''','''        verify_gated_owner_control ();
        const bool controls = zlink::framework::test::same_owner_claim_controls ();
        assert (independent && controls);''',1)
p.write_text(s)
