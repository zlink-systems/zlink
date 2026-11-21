/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/spots/spot_runtime.hpp"

namespace zlink::framework::detail
{

spot_serial_executor_t::spot_serial_executor_t (
  std::shared_ptr<runtime::offload_executor_t> worker_executor,
  runtime::state_lane_t &state_lane,
  runtime::serial_lane_policy_t spot_policy,
  queue_ptr_t spot_queue,
  queue_t::execution_admission_t execution_admission) :
    _worker_executor (std::move (worker_executor)),
    _state_lane (&state_lane),
    _spot_policy (std::move (spot_policy)),
    _execution_admission (std::move (execution_admission))
{
    _spot_queue = std::move (spot_queue);
    if (!_spot_queue && _worker_executor) {
        _spot_queue =
          std::make_shared<queue_t> (*_worker_executor, runtime::serial_execution_queue_options_t{},
                                     queue_t::error_handler_t{}, _spot_policy);
    }
    if (_spot_queue && _execution_admission)
        _spot_queue->set_execution_policy (_execution_admission);
}

void spot_serial_executor_t::attach_preparation (const std::shared_ptr<preparation_t> &preparation)
{
    std::atomic_store (&_preparation, preparation);
    preparation->release_aliases =
      [weak_self = weak_from_this (), weak_preparation = std::weak_ptr (preparation),
       previous = std::move (preparation->release_aliases)] (result_t<void> result) mutable {
          try {
              if (auto self = weak_self.lock ()) {
                  auto expected = weak_preparation.lock ();
                  if (std::atomic_compare_exchange_strong (&self->_preparation, &expected,
                                                           std::shared_ptr<preparation_t>{}))
                      self->update_preparation_policy ({});
              }
          }
          catch (...) {
              if (result)
                  result = detail::current_exception_result<void> ();
          }
          return previous ? previous (std::move (result)) : result;
      };
    update_preparation_policy (preparation);
}

std::shared_ptr<spot_serial_executor_t::preparation_t>
spot_serial_executor_t::begin_preparation (std::shared_ptr<detail::deferred_barrier_t> lifecycle)
{
    auto preparation = std::make_shared<preparation_t> ();
    if (!lifecycle) {
        auto reserved = _spot_queue->reserve_barrier_next ("spot-preparation");
        if (!reserved)
            throw *reserved.error ();
        lifecycle = reserved.value ();
        preparation->admission =
          std::make_shared<const preparation_t::admission_t> (preparation_t::closed_t{});
    } else {
        // An existing lifecycle operation has no activation record to admit.
        // Close retains every pending intent until its own terminal.
        preparation->admission =
          std::make_shared<const preparation_t::admission_t> (preparation_t::first_record_t{});
    }
    preparation->lifecycle = std::move (lifecycle);
    try {
        attach_preparation (preparation);
    }
    catch (...) {
        finish_preparation ();
        throw;
    }
    return preparation;
}

void spot_serial_executor_t::admit_preparation_message (std::shared_ptr<const void> message)
{
    auto preparation = std::atomic_load (&_preparation);
    if (!preparation)
        return;
    auto admission = std::atomic_load (&preparation->admission);
    if (!admission || !std::holds_alternative<preparation_t::closed_t> (*admission))
        return;
    if (std::atomic_compare_exchange_strong (&preparation->admission, &admission,
                                             std::make_shared<const preparation_t::admission_t> (
                                               preparation_t::first_record_t{std::move (message)})))
        update_preparation_policy (preparation);
}

void spot_serial_executor_t::close_preparation_admission ()
{
    if (const auto preparation = std::atomic_load (&_preparation)) {
        std::atomic_store (
          &preparation->admission,
          std::make_shared<const preparation_t::admission_t> (preparation_t::closed_t{}));
        update_preparation_policy (preparation);
    }
}

void spot_serial_executor_t::open_preparation_admission ()
{
    if (const auto preparation = std::atomic_load (&_preparation)) {
        std::atomic_store (&preparation->admission,
                           std::shared_ptr<const preparation_t::admission_t>{});
        update_preparation_policy ({});
    }
}

task_t<void>
spot_serial_executor_t::finish_preparation (detail::deferred_barrier_t::async_work_t cleanup)
{
    auto preparation = std::atomic_load (&_preparation);
    if (!preparation)
        return task_t<void> (result_t<void>::success ());
    auto release = std::exchange (preparation->release_aliases, {});
    if (!release)
        return preparation->terminal.task ();
    auto settle = [preparation, release = std::move (release)] (result_t<void> result) mutable {
        std::atomic_store (&preparation->admission,
                           std::shared_ptr<const preparation_t::admission_t>{});
        // The terminal publisher consumes this operation's alias chain once;
        // every other publisher waits for the same task.
        result = release (std::move (result));
        preparation->terminal.complete (result);
        return result;
    };
    const auto activated = preparation->lifecycle->activate_async (
      [self = shared_from_this (), cleanup = std::move (cleanup), settle] (auto complete) {
          auto finish = [settle, complete] (result_t<void> result) mutable {
              complete (settle (std::move (result)));
          };
          if (!cleanup) {
              finish (result_t<void>::success ());
              return;
          }
          try {
              self->close_preparation_admission ();
              cleanup (finish);
          }
          catch (...) {
              finish (detail::current_exception_result<void> ());
          }
      });
    if (!activated)
        settle (activated);
    return preparation->terminal.task ();
}

bool spot_serial_executor_t::execute_lifecycle (std::string name, std::function<void ()> work) const
{
    return _spot_queue
           && _spot_queue->try_post (
             std::move (name), std::move (work),
             runtime::serial_work_options_t{runtime::serial_work_lane_t::lifecycle});
}

spot_serial_executor_t::actor_executor_ptr_t
spot_serial_executor_t::actor_executor (const std::string &actor_id)
{
    if (!_state_lane || _state_lane->is_on_lane ())
        return actor_executor_on_lane (actor_id);
    return _state_lane->run ([this, &actor_id] { return actor_executor_on_lane (actor_id); })
      .get ();
}

void spot_serial_executor_t::erase_actor_queue (const std::string &actor_id)
{
    if (!_state_lane || _state_lane->is_on_lane ()) {
        erase_actor_queue_on_lane (actor_id);
        return;
    }
    _state_lane->run ([this, &actor_id] { erase_actor_queue_on_lane (actor_id); }).get ();
}

void spot_serial_executor_t::replace_actor_queue (std::string actor_id, queue_ptr_t queue)
{
    if (!_state_lane || _state_lane->is_on_lane ()) {
        replace_actor_queue_on_lane (std::move (actor_id), std::move (queue));
        return;
    }
    _state_lane
      ->run ([this, actor_id = std::move (actor_id), queue = std::move (queue)] () mutable {
          replace_actor_queue_on_lane (std::move (actor_id), std::move (queue));
      })
      .get ();
}

spot_serial_executor_t::queue_ptr_t
spot_serial_executor_t::timer_queue (const std::string &timer_name)
{
    if (!_state_lane || _state_lane->is_on_lane ())
        return timer_queue_on_lane (timer_name);
    return _state_lane->run ([this, &timer_name] { return timer_queue_on_lane (timer_name); })
      .get ();
}

void spot_serial_executor_t::cancel_timer (const std::string &timer_name)
{
    if (!_state_lane || _state_lane->is_on_lane ()) {
        cancel_timer_on_lane (timer_name);
        return;
    }
    _state_lane->run ([this, &timer_name] { cancel_timer_on_lane (timer_name); }).get ();
}

bool spot_serial_executor_t::execute_actor_lifecycle (const std::string &actor_id,
                                                      std::string name,
                                                      queue_t::async_work_t work,
                                                      std::function<void ()> rejected)
{
    runtime::serial_work_options_t options;
    options.lane = runtime::serial_work_lane_t::lifecycle;
    options.holds_application_while_waiting = true;
    options.completion = [rejected] (result_t<void> result) {
        if (!result)
            rejected ();
    };
    const auto gate = uses_spot_execution_gate () ? _spot_queue : queue_ptr_t{};
    auto callback_name = name;
    return execute_actor (
      actor_id, std::move (name),
      [gate, name = std::move (callback_name), work = std::move (work),
       rejected] (auto actor_complete) mutable {
          if (!gate) {
              work (std::move (actor_complete));
              return;
          }
          runtime::serial_work_options_t callback_options;
          callback_options.lane = runtime::serial_work_lane_t::lifecycle;
          callback_options.holds_application_while_waiting = true;
          callback_options.completion = [actor_complete, rejected] (result_t<void> result) {
              if (!result) {
                  rejected ();
                  actor_complete ([] {});
              }
          };
          const auto posted = gate->try_post_async (
            std::move (name),
            [work = std::move (work), actor_complete] (auto spot_complete) mutable {
                work ([spot_complete, actor_complete] (std::function<void ()> finish) {
                    spot_complete ([finish = std::move (finish), actor_complete] {
                        if (finish)
                            finish ();
                        actor_complete ([] {});
                    });
                });
            },
            std::move (callback_options));
          if (!posted) {
              rejected ();
              actor_complete ([] {});
          }
      },
      std::move (options), false, rejected);
}

void spot_serial_executor_t::close (std::vector<task_t<void>> *completions,
                                    queue_t::execution_status_t status)
{
    std::vector<queue_ptr_t> queues;
    if (!_state_lane || _state_lane->is_on_lane ())
        queues = close_on_lane ();
    else
        queues = _state_lane->run ([this] { return close_on_lane (); }).get ();
    for (const auto &queue : queues) {
        if (!queue)
            continue;
        if (completions)
            completions->push_back (queue->cancel_pending_async (status));
        else
            queue->cancel_pending (status);
    }
}

spot_serial_executor_t::queue_t::application_start_policy_t
spot_serial_executor_t::preparation_policy (const std::shared_ptr<preparation_t> &preparation)
{
    if (!preparation || !std::atomic_load (&preparation->admission))
        return {};
    return [weak_preparation =
              std::weak_ptr (preparation)] (const std::shared_ptr<const void> &message) noexcept {
        const auto preparation = weak_preparation.lock ();
        const auto admission = preparation ? std::atomic_load (&preparation->admission) : nullptr;
        if (!admission)
            return true;
        const auto first = std::get_if<preparation_t::first_record_t> (admission.get ());
        return first && first->message && first->message == message;
    };
}

void spot_serial_executor_t::update_preparation_policy (std::shared_ptr<preparation_t> preparation)
{
    _spot_queue->set_application_start_policy (preparation_policy (preparation));
    auto update = [self = shared_from_this (), preparation = std::move (preparation)] {
        for (const auto &[_, executor] : self->_actor_executors)
            executor->queue ()->set_application_start_policy (preparation_policy (preparation));
        for (const auto &[_, queue] : self->_timer_queues)
            queue->set_application_start_policy (preparation_policy (preparation));
    };
    if (!_state_lane || _state_lane->is_on_lane ())
        update ();
    else if (!_state_lane->try_post (std::move (update)))
        throw framework_exception_t (framework_error_kind_t::shutting_down,
                                     "Spot preparation owner lane is unavailable");
}

spot_serial_executor_t::actor_executor_ptr_t
spot_serial_executor_t::actor_executor_on_lane (const std::string &actor_id)
{
    assert_on_lane ();
    auto &executor = _actor_executors[actor_id];
    if (!executor && _worker_executor) {
        executor =
          std::make_shared<actor_executor_t> (_worker_executor, queue_ptr_t{}, _spot_queue);
        executor->queue ()->set_application_start_policy (
          preparation_policy (std::atomic_load (&_preparation)));
        if (_execution_admission)
            executor->queue ()->set_execution_policy (_execution_admission);
        publish_actor_executor_snapshot ();
    }
    return executor;
}

void spot_serial_executor_t::replace_actor_queue_on_lane (std::string actor_id, queue_ptr_t queue)
{
    assert_on_lane ();
    _actor_executors.insert_or_assign (
      actor_id,
      std::make_shared<actor_executor_t> (_worker_executor, std::move (queue), _spot_queue));
    if (_execution_admission)
        _actor_executors.at (actor_id)->queue ()->set_execution_policy (_execution_admission);
    _actor_executors.at (actor_id)->queue ()->set_application_start_policy (
      preparation_policy (std::atomic_load (&_preparation)));
    publish_actor_executor_snapshot ();
}

spot_serial_executor_t::queue_ptr_t
spot_serial_executor_t::timer_queue_on_lane (const std::string &timer_name)
{
    assert_on_lane ();
    auto &queue = _timer_queues[timer_name];
    if (!queue && _worker_executor) {
        queue = std::make_shared<queue_t> (
          *_worker_executor, runtime::serial_execution_queue_options_t{},
          queue_t::error_handler_t{}, runtime::serial_lane_policy_t::actor_delivery ());
        queue->set_application_start_policy (preparation_policy (std::atomic_load (&_preparation)));
        if (_execution_admission)
            queue->set_execution_policy (
              [spot = _spot_queue] (const queue_t &) { return spot->execution_status (); });
    }
    return queue;
}

std::vector<spot_serial_executor_t::queue_ptr_t>
spot_serial_executor_t::timer_queues_on_lane () const
{
    assert_on_lane ();
    std::vector<queue_ptr_t> result;
    result.reserve (_timer_queues.size ());
    for (const auto &[_, queue] : _timer_queues)
        result.push_back (queue);
    return result;
}

void spot_serial_executor_t::cancel_timer_on_lane (const std::string &timer_name)
{
    assert_on_lane ();
    const auto found = _timer_queues.find (timer_name);
    if (found == _timer_queues.end ())
        return;
    if (found->second)
        found->second->cancel_pending ();
    _timer_queues.erase (found);
}

std::vector<spot_serial_executor_t::queue_ptr_t> spot_serial_executor_t::close_on_lane ()
{
    assert_on_lane ();
    std::vector<queue_ptr_t> queues;
    queues.reserve (1 + _actor_executors.size () + _timer_queues.size ());
    if (_spot_queue)
        queues.push_back (_spot_queue);
    for (const auto &[_, executor] : _actor_executors)
        if (executor)
            queues.push_back (executor->queue ());
    for (const auto &[_, queue] : _timer_queues)
        queues.push_back (queue);
    _actor_executors.clear ();
    _timer_queues.clear ();
    publish_actor_executor_snapshot ();
    return queues;
}

void spot_serial_executor_t::publish_actor_executor_snapshot ()
{
    auto snapshot = std::make_shared<actor_executor_map_t> (_actor_executors);
    std::shared_ptr<const actor_executor_map_t> published = std::move (snapshot);
    std::atomic_store_explicit (&_actor_executor_snapshot, std::move (published),
                                std::memory_order_release);
}

} // namespace zlink::framework::detail
