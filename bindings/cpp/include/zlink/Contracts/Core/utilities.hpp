/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "../Errors/errors.hpp"

#include <cstdint>
#include <functional>
#include <memory>

namespace zlink
{

class stopwatch_t
{
  public:
    stopwatch_t ();
    ~stopwatch_t ();

    stopwatch_t (stopwatch_t &&other_) noexcept;
    stopwatch_t &operator= (stopwatch_t &&other_) noexcept;

    stopwatch_t (const stopwatch_t &) = delete;
    stopwatch_t &operator= (const stopwatch_t &) = delete;

    bool valid () const noexcept;
    uint64_t intermediate () const;
    uint64_t stop ();
    void close ();

  private:
    void *_handle = nullptr;
};

class atomic_counter_t
{
  public:
    atomic_counter_t ();
    ~atomic_counter_t ();

    atomic_counter_t (atomic_counter_t &&other_) noexcept;
    atomic_counter_t &operator= (atomic_counter_t &&other_) noexcept;

    atomic_counter_t (const atomic_counter_t &) = delete;
    atomic_counter_t &operator= (const atomic_counter_t &) = delete;

    bool valid () const noexcept;
    void set (int value_);
    int increment ();
    int decrement ();
    int value () const;
    void close ();

  private:
    void *_handle = nullptr;
};

class thread_t
{
  public:
    struct state;

    explicit thread_t (std::function<void ()> task_);
    ~thread_t ();

    thread_t (thread_t &&other_) noexcept;
    thread_t &operator= (thread_t &&other_) noexcept;

    thread_t (const thread_t &) = delete;
    thread_t &operator= (const thread_t &) = delete;

    bool valid () const noexcept;
    void join ();
    void close ();

  private:
    void *_handle = nullptr;
    std::unique_ptr<state> _state;
};

} // namespace zlink
