from pathlib import Path
p=Path('/root/worktree/zlink-3867s/framework/languages/cpp/framework/src/runtime/foundation/operation_registry.cpp')
s=p.read_text()
# Give a terminal holder ownership of the extracted registry node, independently of C2's index ref.
s=s.replace('''class operation_registry_state_t :''','''struct operation_completion_deleter_t
{
    void operator() (operation_completion_item_t *item) const noexcept;
};
using operation_completion_ptr_t =
  std::unique_ptr<operation_completion_item_t, operation_completion_deleter_t>;

class operation_registry_state_t :''',1)
# Move definitions that the inline pending item needs ahead of the state class.
start=s.index('struct operation_registry_drain_state_t\n{');end=s.index('\nstruct operation_completion_chain_t',start)
item=s[start:end]
item=item.replace('    void operator() () const noexcept { registry->cancel (id); }','    void operator() () const noexcept;')
item=item.replace('operation_registry_state_t::callback_t','operation_registry_t::callback_t')
item=item.replace('''    ~operation_completion_item_t ()''','''    operation_completion_item_t () = default;
    operation_completion_item_t (operation_completion_item_t &&) noexcept = default;
    operation_completion_item_t &operator= (operation_completion_item_t &&) noexcept = default;

    ~operation_completion_item_t ()''')
item=item.replace('std::optional<std::stop_callback<operation_cancellation_t>> cancellation;', 'std::unique_ptr<std::stop_callback<operation_cancellation_t>> cancellation;')
item=item.replace('std::unique_ptr<operation_completion_item_t> next;', 'operation_completion_ptr_t next;')
s=s[:start]+s[end:]
pos=s.index('class operation_registry_state_t :');s=s[:pos]+item+'\n'+s[pos:]
s=s.replace('''    struct pending_t
    {
        std::unique_ptr<operation_completion_item_t> completion;''','''    struct pending_t : operation_completion_item_t
    {
        pending_t () = default;
        explicit pending_t (operation_completion_item_t completion) :
            operation_completion_item_t (std::move (completion))
        {
        }
        pending_t (pending_t &&other) noexcept :
            operation_completion_item_t (std::move (other)),
            target_routing_id (std::move (other.target_routing_id)),
            id (other.id), deadline (other.deadline), request (std::move (other.request))
        {
        }
        // Terminal-table/dispatcher ownership and the C2 index each retain one
        // reference. Extract transfers the former without changing the latter.
        std::atomic_uint references{2};
        alignas (std::max_align_t) std::byte extracted_node[4 * sizeof (void *)];''',1)
start=s.index('    template <typename Map> static auto find_pending');end=s.index('    bool index_deadline',start)
s=s[:start]+'''    template <typename Map> static auto find_pending (Map &pending, const call_id_t &id)
    {
        return pending.find (id);
    }
    bool is_pending (const pending_t &pending) const
    {
        const auto found = _entries.find (pending.id);
        return found != _entries.end () && &found->second == &pending;
    }
    operation_completion_ptr_t take_completion (pending_t &pending);
    static void release_node (pending_t &pending) noexcept;
    bool take (const call_id_t &id, operation_completion_ptr_t &completion);
'''+s[end:]
s=s.replace('''    // C1 owns completion take. A taken entry remains as index storage until
    // management visits it; a reused identity may coexist with its taken entry.
    std::unordered_multimap<call_id_t, pending_t, id_hash_t> _entries;
    // Observation only: terminal authority remains the completion slot above.
    // Map size also includes taken entries retained solely as index storage.
    std::size_t _pending_count = 0;''','''    // C1 table membership is the only terminal authority. C2 retains references
    // to extracted nodes until its next visit, independently of callback delivery.
    using entries_t = std::unordered_map<call_id_t, pending_t, id_hash_t>;
    entries_t _entries;''',1)
pos=s.index('\n\nstruct operation_completion_chain_t')
s=s[:pos]+'''

void operation_cancellation_t::operator() () const noexcept { registry->cancel (id); }

operation_completion_ptr_t operation_registry_state_t::take_completion (pending_t &pending)
{
    using node_t = entries_t::node_type;
    static_assert (sizeof (node_t) <= sizeof (pending.extracted_node));
    static_assert (alignof (node_t) <= alignof (pending_t));
    auto node = _entries.extract (pending.id);
    assert (!node.empty () && &node.mapped () == &pending);
    std::construct_at (reinterpret_cast<node_t *> (pending.extracted_node), std::move (node));
    return operation_completion_ptr_t (&pending);
}

void operation_registry_state_t::release_node (pending_t &pending) noexcept
{
    if (pending.references.fetch_sub (1, std::memory_order_acq_rel) != 1)
        return;
    using node_t = entries_t::node_type;
    auto *storage = std::launder (reinterpret_cast<node_t *> (pending.extracted_node));
    auto node = std::move (*storage);
    std::destroy_at (storage);
    // The local node handle destroys its own pending value after the last borrow.
}

void operation_completion_deleter_t::operator() (operation_completion_item_t *item) const noexcept
{
    if (!item)
        return;
    auto &pending = *static_cast<operation_registry_state_t::pending_t *> (item);
    // A retained index reference must not retain callback captures or reply bytes.
    item->callback = nullptr;
    item->payload.reset ();
    pending.request.reset ();
    auto drain = std::move (item->drain_state);
    operation_registry_state_t::release_node (pending);
    if (drain)
        drain->release ();
}
'''+s[pos:]
s=s.replace('std::unique_ptr<operation_completion_item_t>', 'operation_completion_ptr_t')
# Prepared captures and cancellation subscription remain outside C1's lock.
s=s.replace('''    pending_t prepared;''','''    pending_t prepared (operation_completion_item_t (
      std::move (callback), _drain_state, detail::borrow_completion_dispatcher ()));''',1)
s=s.replace('''    prepared.completion = std::make_unique<operation_completion_item_t> (
      std::move (callback), _drain_state, detail::borrow_completion_dispatcher ());
    prepared.completion->request_metric = std::move (request_metric);''','''    prepared.request_metric = std::move (request_metric);''',1)
s=s.replace('''        prepared.completion->cancellation.emplace (cancellation,
                                                   operation_cancellation_t{this, id});''','''        prepared.cancellation = std::make_unique<std::stop_callback<operation_cancellation_t>> (
          cancellation, operation_cancellation_t{this, id});''',1)
s=s.replace('''        auto position = _entries.emplace (id, pending_t{});
        position->second = std::move (prepared);
        auto *entry = &position->second;
        ++_pending_count;
        entry->completion->request_metric.start ();''','''        auto position = _entries.emplace (id, std::move (prepared)).first;
        auto *entry = &position->second;
        entry->request_metric.start ();''',1)
# An index publication owns its reference even after C1 extraction.
s=s.replace('''                        const auto [begin, end] = self->_entries.equal_range (id);
                        const auto found = std::find_if (
                          begin, end, [entry] (const auto &item) { return &item.second == entry; });
                        if (found == end || found->second.deadline_slot != deadline_slot_count)
                            return;
                        earlier = self->index_deadline (id, found->second.deadline, found->second);''','''                        if (!self->is_pending (*entry)) {
                            release_node (*entry);
                            return;
                        }
                        earlier = self->index_deadline (id, entry->deadline, *entry);''',1)
# A rejected publication never indexed the node. Hold its terminal ownership outside C1.
s=s.replace('''    std::exception_ptr publication_error;
    {''','''    std::exception_ptr publication_error;
    operation_completion_ptr_t rejected;
    {''',1)
s=s.replace('''                prepared.completion = take_completion (position->second);
                _entries.erase (position);''','''                rejected = take_completion (position->second);
                release_node (position->second);''',1)
# Clean subscription before a rejected holder can release the registry drain.
s=s.replace('''    if (publication_error)
        std::rethrow_exception (publication_error);''','''    if (rejected)
        rejected->cancellation.reset ();
    if (publication_error)
        std::rethrow_exception (publication_error);''',1)
# Publication rejected by owner close still has its reference until this lambda executes.
s=s.replace('''                        if (self->_closed)
                            return;
                        // Compare addresses''','''                        if (self->_closed) {
                            release_node (*entry);
                            return;
                        }
                        // Compare addresses''',1)
start=s.index('    const auto [begin, end] = _entries.equal_range (pending.id);');end=s.index('\n}',start)
s=s[:start]+'''    release_node (pending);'''+s[end:]
# Fail target extraction invalidates the current iterator.
s=s.replace('''            if (!entry->second.completion || entry->second.target_routing_id != target_routing_id)''','''            if (entry->second.target_routing_id != target_routing_id)''',1)
s=s.replace('''            completions.append (take_completion (entry->second), terminal);
            ++entry;''','''            auto &pending = (entry++)->second;
            completions.append (take_completion (pending), terminal);''',1)
s=s.replace('''            if (current->completion && current->deadline <= now)
                completion = take_completion (*current);
            const bool live = bool (current->completion);''','''            const bool live = is_pending (*current) && current->deadline > now;
            if (!live && is_pending (*current))
                completion = take_completion (*current);''',1)
s=s.replace('''            for (auto &[id, pending] : _entries) {
                static_cast<void> (id);
                if (pending.completion)
                    completions.append (take_completion (pending), operation_terminal_t::shutdown);
            }''','''            while (!_entries.empty ())
                completions.append (take_completion (_entries.begin ()->second),
                                    operation_terminal_t::shutdown);''',1)
# Extracted entries still have an index even though the terminal table is empty.
s=s.replace('''            if (!_entries.empty ()) {''','''            if (_index_size != 0) {''',1)
s=s.replace('''    return _pending_count;''','''    return _entries.size ();''',1)
# set next to an explicit null deleter pointer (default constructible)
s=s.replace('#include <bit>','#include <bit>\n#include <cassert>')
p.write_text(s)
