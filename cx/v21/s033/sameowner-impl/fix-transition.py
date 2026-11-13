from pathlib import Path
p=Path('framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.hpp');s=p.read_text().replace('changed, begin, end','changed, begin, consumed, end').replace('const claimed_t &claimed,\n                        std::unique_lock','const claimed_t &claimed,\n                        bool queued,\n                        std::unique_lock');p.write_text(s)
p=Path('framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.cpp');s=p.read_text().replace('''    if (step == ready_step_t::end) {''','''    if (step == ready_step_t::consumed) {
        domain.ready.remove (owner);
        queue.phase = owner_phase_t::idle;
        return true;
    }
    if (step == ready_step_t::end) {''').replace('claimed, lock);','claimed, true, lock);',1)
a=s.index('service_mailbox_t::try_claim_owner');s=s[:a]+s[a:].replace('claimed, lock);','claimed, false, lock);',1)
s=s.replace('''                                       const claimed_t &claimed,
                                       std::unique_lock''','''                                       const claimed_t &claimed,
                                       bool queued,
                                       std::unique_lock''')
s=s.replace('''    if (domain_value != service_mailbox_domain_t::application || !_state->application_ready)
        refresh_ready_locked (source, owner, ready_step_t::begin);''','''    if (queue.phase != owner_phase_t::draining
        && (queued || domain_value != service_mailbox_domain_t::application
            || !_state->application_ready))
        refresh_ready_locked (source, owner, ready_step_t::consumed);''')
s=s.replace('''    if (notify)
        notify_application_ready ();
    return claim;''','''    if (notify) {
        try {
            notify_application_ready ();
        }
        catch (...) {
            release (*claim);
            throw;
        }
    }
    return claim;''')
p.write_text(s)
