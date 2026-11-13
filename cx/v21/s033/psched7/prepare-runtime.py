from pathlib import Path
root=Path('/root/worktree/zlink-v21-s033/psched5/framework/languages/cpp/framework/src/runtime/dispatch')
p=root/'offload_executor.hpp';s=p.read_text()
s=s.replace('    drain_source_t::record_t take_drain_locked (std::deque<work_item_t>::iterator entry);\n','')
s=s.replace('    void\n    reserve_workers_locked (bool grow, notifications_t &notifications, worker_t *current = nullptr);','''    enum class reservation_t
    {
        publication,
        take,
        wait
    };
    drain_source_t::record_t take_drain_locked (std::deque<work_item_t>::iterator entry,
                                               notifications_t &notifications);
    void reserve_workers_locked (reservation_t reason,
                                  notifications_t &notifications,
                                  worker_t *current = nullptr);''')
p.write_text(s)
p=root/'offload_executor.cpp';s=p.read_text()
s=s.replace('reserve_workers_locked (true, notifications)', 'reserve_workers_locked (reservation_t::publication, notifications)')
s=s.replace('reserve_workers_locked (false, notifications', 'reserve_workers_locked (reservation_t::wait, notifications')
s=s.replace('offload_executor_t::take_drain_locked (std::deque<work_item_t>::iterator entry)','''offload_executor_t::take_drain_locked (std::deque<work_item_t>::iterator entry,
                                       notifications_t &notifications)''')
s=s.replace('    else\n        std::rotate (entry, entry + 1, _queue.end ());\n    return record;','''    else {
        std::rotate (entry, entry + 1, _queue.end ());
        reserve_workers_locked (reservation_t::take, notifications);
    }
    return record;''')
s=s.replace('    std::shared_ptr<drain_source_t> selected;\n    drain_source_t::record_t record;', '    std::shared_ptr<drain_source_t> selected;\n    drain_source_t::record_t record;\n    notifications_t notifications;')
s=s.replace('record = take_drain_locked (found);', 'record = take_drain_locked (found, notifications);')
s=s.replace("""    return record;
}

// Only publication can grow the existing elastic pool. A take never wakes or
// starts another worker. The worker phase is the sole wake-reservation state.
void offload_executor_t::reserve_workers_locked (bool grow,""",'''    notify_workers (notifications);
    return record;
}

// Publication counts ready entries. A take with records remaining chains at most
// one worker reservation, using the worker phase as the sole reservation state.
void offload_executor_t::reserve_workers_locked (reservation_t reason,''')
s=s.replace('    demand += std::min (_queue.size (), _max_worker_count - demand);','''    demand += std::min (_queue.size (), _max_worker_count - demand);
    if (reason == reservation_t::take)
        demand = std::min (demand, std::size_t{1});''')
s=s.replace('if (grow && available < demand', 'if (reason != reservation_t::wait && available < demand')
s=s.replace('record = take_drain_locked (_queue.begin ());', 'record = take_drain_locked (_queue.begin (), notifications);')
p.write_text(s)
