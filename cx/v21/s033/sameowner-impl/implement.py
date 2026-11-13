from pathlib import Path
r=Path('framework/languages/cpp/framework/src/runtime')
p=r/'mesh/service_mailbox.hpp'
s=p.read_text().replace('    service_mailbox_t () = default;','    service_mailbox_t () = default;\n    explicit service_mailbox_t (std::string channel_owner);')
s=s.replace('    static std::string application_owner (const host::ready_record_t &owner);','    static std::string application_owner (const host::ready_record_t &owner);\n    static std::optional<host::owner_kind_t> application_owner_kind (std::string_view owner);')
a=s.index('    // A claim is the front prefix of records;')
b=s.index('    domain_t &domain',a)
s=s[:a]+'''    using records_t = std::list<service_mailbox_record_t>;
    struct owner_queue_t
    {
        explicit owner_queue_t (std::optional<host::owner_kind_t> kind) : kind (kind) {}
        const std::optional<host::owner_kind_t> kind;
        records_t records;
        records_t::iterator unclaimed = records.end ();
        owner_phase_t phase = owner_phase_t::idle;
        bool independent () const noexcept
        {
            return kind == host::owner_kind_t::node || kind == host::owner_kind_t::channel;
        }
    };

    struct domain_t
    {
        std::unordered_map<std::string, owner_queue_t> owners;
        std::list<std::string> ready;
        std::size_t messages = 0;
        std::size_t bytes = 0;
    };

    static bool eligible (const owner_queue_t &queue);
    // All reservation and drain state changes pass through this transition.
    enum class ready_step_t { changed, begin, end };
    static bool refresh_ready_locked (domain_t &domain, const std::string &owner,
                                      ready_step_t step = ready_step_t::changed);
''' +s[b:]
s=s.replace('    std::vector<service_mailbox_record_t *> records;','    std::vector<std::list<service_mailbox_record_t>::iterator> records;')
s=s.replace('// One claim of an owner\'s FIFO prefix.', '// One claim of an owner\'s FIFO records.')
p.write_text(s)
p=r/'mesh/service_mailbox.cpp';s=p.read_text();pos=s.index('service_mailbox_t::domain_t &service_mailbox_t::domain')
s=s[:pos]+'''service_mailbox_t::service_mailbox_t (std::string channel_owner)
{
    _state->application.owners.try_emplace (std::move (channel_owner), host::owner_kind_t::channel);
}

std::optional<host::owner_kind_t>
service_mailbox_t::application_owner_kind (std::string_view owner)
{
    if (owner == "node")
        return host::owner_kind_t::node;
    if (owner.starts_with ("channel:"))
        return host::owner_kind_t::channel;
    if (owner.starts_with (detail::activation_admission_t::actor_key_prefix))
        return host::owner_kind_t::actor;
    if (owner.starts_with (detail::activation_admission_t::spot_key_prefix))
        return host::owner_kind_t::spot;
    return std::nullopt;
}

bool service_mailbox_t::eligible (const owner_queue_t &queue)
{
    return queue.unclaimed != queue.records.end ()
           && (queue.independent () || queue.records.begin () == queue.unclaimed);
}

bool service_mailbox_t::refresh_ready_locked (domain_t &domain, const std::string &owner,
                                              ready_step_t step)
{
    const auto found = domain.owners.find (owner);
    if (found == domain.owners.end ())
        return false;
    auto &queue = found->second;
    if (step == ready_step_t::begin) {
        if (queue.phase != owner_phase_t::ready)
            return false;
        domain.ready.remove (owner);
        queue.phase = owner_phase_t::draining;
        return true;
    }
    if (step == ready_step_t::end) {
        // An independent drain owns only its take; its end cannot revoke a successor.
        if (queue.independent () || queue.phase != owner_phase_t::draining)
            return false;
        queue.phase = owner_phase_t::idle;
    }
    if (!queue.independent () && queue.phase == owner_phase_t::draining)
        return false;
    const auto next = eligible (queue) ? owner_phase_t::ready
                      : queue.records.empty () ? owner_phase_t::idle
                                               : owner_phase_t::retained;
    if (queue.phase == owner_phase_t::ready && next != owner_phase_t::ready)
        domain.ready.remove (owner);
    if (queue.phase != owner_phase_t::ready && next == owner_phase_t::ready)
        domain.ready.push_back (owner);
    queue.phase = next;
    // Explicit raw Channel registrations survive an empty queue. Canonical Mesh
    // owners can be reconstructed by this same registration owner on their next enqueue.
    if (queue.records.empty () && queue.kind == application_owner_kind (owner))
        domain.owners.erase (found);
    return next == owner_phase_t::ready;
}

''' +s[pos:]
s=s.replace('    auto &queue = target.owners[owner];','''    auto &queue = target.owners.try_emplace (
      owner, record.domain == service_mailbox_domain_t::application
               ? application_owner_kind (owner) : std::nullopt).first->second;''')
s=s.replace('    queue.records.push_back (std::move (record));\n    queue.bytes += retained;\n    ++queue.messages;','''    const auto inserted = queue.records.insert (queue.records.end (), std::move (record));
    if (queue.unclaimed == queue.records.end ())
        queue.unclaimed = inserted;''')
s=s.replace('''    if (queue.phase == owner_phase_t::idle) {
        queue.phase = owner_phase_t::ready;
        target.ready.push_back (owner);
    }''','    refresh_ready_locked (target, owner);')
a=s.index('    while (!source.ready.empty ())');b=s.index('    return nullptr;\n}',a)
s=s[:a]+'''    while (!source.ready.empty ()) {
        const auto owner = source.ready.front ();
        return claim_owner_locked (source, domain_value, owner, message_budget, byte_budget,
                                   claimed, lock);
    }
'''+s[b:]
a=s.index('    const auto found = source.owners.find (owner);',s.index('service_mailbox_t::try_claim_owner'));b=s.index('\n}',a)
s=s[:a]+'''    return claim_owner_locked (source, domain_value, owner, message_budget, byte_budget,
                               claimed, lock);'''+s[b:]
s=s.replace('''found->second.active_messages != 0
        || found->second.records.empty ()''','''!eligible (found->second)''')
s=s.replace('    auto &queue = found->second;\n    auto claim =', '''    auto &queue = found->second;
    if (queue.independent () && !claimed)
        throw std::invalid_argument ("independent mailbox claim requires original transfer");
    auto claim =''')
s=s.replace('    const auto limit = std::min (message_budget, queue.records.size ());','    const auto limit = queue.independent () ? 1 : std::min (message_budget, queue.records.size ());')
s=s.replace('''    for (auto &slot : queue.records) {''','''    auto unclaimed = queue.unclaimed;
    for (; unclaimed != queue.records.end (); ++unclaimed) {
        auto &slot = *unclaimed;''',1)
s=s.replace('        claim->records.push_back (&slot);','        claim->records.push_back (unclaimed);')
a=s.index('    queue.bytes -= claim->claimed_bytes;');b=s.index('    // A consumer allocation',a)
s=s[:a]+'''    queue.unclaimed = unclaimed;
    source.bytes -= claim->claimed_bytes;
    source.messages -= claim->claimed_messages;
    // A bound application drain may already have published a successor. Keep that
    // reservation while the current worker consumes fast records within its W5 budget.
    if (domain_value != service_mailbox_domain_t::application || !_state->application_ready)
        refresh_ready_locked (source, owner, ready_step_t::begin);
'''+s[b:]
s=s.replace('''    pending.release ();
    return claim;''','''    if (queue.independent () && has_original (*claim->records.front ()))
        throw std::logic_error ("independent claim callback did not transfer its original");
    refresh_ready_locked (source, owner);
    pending.release ();
    const bool notify = domain_value == service_mailbox_domain_t::application
                        && !_state->application_receive_turn;
    lock.unlock ();
    if (notify)
        notify_application_ready ();
    return claim;''')
s=s.replace('    auto *slot = handle.claim->records[handle.index];','    auto *slot = &*handle.claim->records[handle.index];')
s=s.replace('    const auto *previous = handle.claim->records[handle.index - 1];','    const auto *previous = &*handle.claim->records[handle.index - 1];')
a=s.index('        const auto claimed = std::min (queue.active_messages');b=s.index('    }\n    if (cut)',a)
s=s[:a]+'''        for (auto slot = queue.records.begin (); slot != queue.unclaimed; ++slot)
            if (has_original (*slot))
                records.push_back (transfer_locked (*slot));
        for (auto slot = queue.unclaimed; slot != queue.records.end (); ++slot) {
            const auto bytes = retained_bytes (*slot);
            source.bytes -= bytes;
            --source.messages;
            records.push_back (transfer_locked (*slot));
        }
        queue.records.erase (queue.unclaimed, queue.records.end ());
        queue.unclaimed = queue.records.end ();
        refresh_ready_locked (source, owners[index]);
'''+s[b:]
a=s.index('    // Every slot of the prefix');b=s.index('    return &target',a)
s=s[:a]+'''    // A younger claim may settle first. Only this claim's own slots leave the FIFO.
    for (const auto slot : claim.records)
        queue.records.erase (slot);
    refresh_ready_locked (target, claim.owner);
'''+s[b:]
a=s.index('    const auto found = _state->application.owners.find (owner);',s.index('bool service_mailbox_t::begin_application_drain'));b=s.index('\n}',a)
s=s[:a]+'''    return refresh_ready_locked (_state->application, owner, ready_step_t::begin);'''+s[b:]
a=s.index('    const auto found = _state->application.owners.find (owner);',s.index('void service_mailbox_t::end_application_drain'));b=s.index('    const bool notify',a)
s=s[:a]+'''    refresh_ready_locked (_state->application, owner, ready_step_t::end);
'''+s[b:]
p.write_text(s)
p=r/'client_server/raw_client_server_owner.cpp';s=p.read_text().replace('    _mailbox (),','    _mailbox (_options.descriptor.channel_name),');p.write_text(s)
p=r/'stateful/public_host_runtime.cpp';s=p.read_text();a=s.index('std::optional<owner_kind_t> stateful_application_owner');b=s.index('\n} // namespace',a);s=s[:a]+s[b:];s=s.replace('stateful_application_owner (claim->owner)','mesh::service_mailbox_t::application_owner_kind (claim->owner)');s=s.replace('if (stateful && mesh::service_mailbox_t::application_owner_kind (claim->owner))','if (stateful && (mesh::service_mailbox_t::application_owner_kind (claim->owner) == owner_kind_t::actor\n                         || mesh::service_mailbox_t::application_owner_kind (claim->owner) == owner_kind_t::spot))');p.write_text(s)
p=r/'client_server/client_server_location_runtime.cpp';s=p.read_text().replace('original.emplace (std::move (*claim->records.front ()));','original.emplace (std::move (*claim->records.front ()));\n                  claim->records.front ()->application.reset ();');p.write_text(s)
