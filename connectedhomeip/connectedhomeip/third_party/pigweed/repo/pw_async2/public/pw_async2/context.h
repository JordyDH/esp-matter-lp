// Copyright 2025 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.
#pragma once

namespace pw::async2 {

class Dispatcher;
class Waker;

/// Context for an asynchronous ``Task``.
///
/// This object contains resources needed for scheduling asynchronous work,
/// such as the current ``Dispatcher`` and the ``Waker`` for the current task.
///
/// ``Context`` s are most often created by ``Dispatcher`` s, which pass them
/// into ``Task::Pend``.
class Context {
 public:
  /// Creates a new ``Context`` containing the currently-running ``Dispatcher``
  /// and a ``Waker`` for the current ``Task``.
  Context(Dispatcher& dispatcher, Waker& waker)
      : dispatcher_(&dispatcher), waker_(&waker) {}

  /// The ``Dispatcher`` on which the current ``Task`` is executing.
  ///
  /// This can be used for spawning new tasks using
  /// ``dispatcher().Post(task);``.
  Dispatcher& dispatcher() { return *dispatcher_; }

  /// Queues the current ``Task::Pend`` to run again in the future, possibly
  /// after other work is performed.
  ///
  /// This may be used by ``Task`` implementations that wish to provide
  /// additional fairness by yielding to the dispatch loop rather than perform
  /// too much work in a single iteration.
  ///
  /// This is semantically equivalent to calling:
  ///
  /// ```
  /// Waker waker;
  /// PW_ASYNC_STORE_WAKER(cx, waker, ...);
  /// std::move(waker).Wake();
  /// ```
  void ReEnqueue();

  /// INTERNAL-ONLY: users should use the `PW_ASYNC_STORE_WAKER` macro instead.
  ///
  /// Saves a ``Waker`` into ``waker_out`` which, when awoken, will cause the
  /// current task to be ``Pend``'d by its dispatcher.
  void InternalStoreWaker(Waker& waker_out);

 private:
  Dispatcher* dispatcher_;
  Waker* waker_;
};

}  // namespace pw::async2
