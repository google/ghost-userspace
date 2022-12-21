// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

// A small library of support functionality with no uapi dependencies that can
// be shared between agent and clients.  This is picked up by both the agent and
// clients.

#ifndef GHOST_LIB_BASE_H_
#define GHOST_LIB_BASE_H_

#include <execinfo.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <sys/capability.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "absl/base/call_once.h"
#include "absl/container/flat_hash_map.h"
#include "absl/flags/declare.h"
#include "absl/flags/flag.h"
#include "absl/memory/memory.h"
#include "absl/status/statusor.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "lib/logging.h"

ABSL_DECLARE_FLAG(std::string, ghost_procfs_prefix);
ABSL_DECLARE_FLAG(bool, emit_fork_warnings);

namespace ghost {

// Returns the path to access the requested procfs file (eg. since proc may not
// be mounted at root).
std::string GetProc(const std::string& procfs_path);

inline pid_t GetTID();
void Exit(int code);
size_t GetFileSize(int fd);
void SpinFor(absl::Duration remaining);
void PrintBacktrace(FILE* f, void* uctx = nullptr);
bool CapHas(cap_value_t cap);

// This is useful for reading non-atomic variables that may be changed by the
// kernel or by other threads. There are three main advantages to wrapping a
// read to a variable `x` in `READ_ONCE()`:
//
// 1. The read is performed as though `x` were volatile. In other words, a read
// to `x` actually occurs; you do not need to worry that the compiler has cached
// the value of `x` in a register and that you are actually using a stale value
// of `x`.
//
// 2. Without `READ_ONCE` and `WRITE_ONCE`, some compilers may split up a
// read/write to/from a variable into multiple reads/writes (i.e., "load
// tearing" and "store tearing"). Thus, if one thread is writing to a variable
// while another thread is reading from the variable, the reader may load a
// corrupted value. Wrapping a read to `x` in `READ_ONCE()` ensures that the
// entire read happens in one operation rather than be split up into multiple
// read operations.
//
// 3. The variable will not be reloaded if you access it later on without
// wrapping it in `READ_ONCE()`.
// Example:
// T* y = READ_ONCE(x);
// if (y) {
//   y->DoSomething();  // Without a `READ_ONCE()` above, the compiler is
//                      // allowed to reload `x` here (i.e., the compiler is not
//                      // required to create a variable `y` and copy `x` into
//                      // `y` if you do not use `READ_ONCE()` above). If this
//                      // were to happen, the pointer could then be a nullptr
//                      // here and the process would segfault.
// }
//
// See `Documentation/memory-barriers.txt` in the Linux kernel for more details.
template <typename T>
inline T READ_ONCE(const T& x) {
  return reinterpret_cast<const std::atomic<T>*>(&x)->load(
      std::memory_order_relaxed);
}

template <typename T>
inline void WRITE_ONCE(T& x, T val) {
  reinterpret_cast<std::atomic<T>*>(&x)->store(val, std::memory_order_relaxed);
}

// You can pass the `-Wunused-result` flag to some compilers, which generates a
// warning if you do not use the return value of some functions. There may be
// functions whose return values we do want to ignore, so we can wrap each
// function call in the function below to suppress the warning.
//
// Example:
// int SomeFunction() { ... }
//
// SomeFunction();  // Generates a warning.
//
// IGNORE_RETURN_VALUE(SomeFunction());  // Does not generate a warning.
template <typename T>
inline void IGNORE_RETURN_VALUE(const T& x) {}

inline absl::Time MonotonicNow() {
  timespec ts;

  CHECK_EQ(clock_gettime(CLOCK_MONOTONIC, &ts), 0);
  return absl::TimeFromTimespec(ts);
}

// Returns the TID (thread identifier) of the calling thread.
inline pid_t GetTID() {
  static thread_local int tid = syscall(__NR_gettid);
  return tid;
}

// Returns the raw GTID (ghOSt thread identifier) of the calling thread.
//
// A ghost tid is a 64-bit identifier for a task (as opposed to a TID
// that can be up to 22 bits). The extra bits allow an agent to distinguish
// between incarnations of the same tid (due to tid reuse). It is always
// possible to get the linux TID associated with a GTID and interact with
// it using traditional linux tools (e.g. ps, top, gdb, /proc/<pid>).
//
// Most callers should never need to call this function (preferring
// Gtid::Current() since it caches the gtid in a thread-local var).
absl::StatusOr<int64_t> GetGtid();

// Issues the equivalent of an x86 `pause` instruction on the target
// architecture. This is generally useful to call in the body of a spinlock loop
// since it reduces power consumption and cache contention.
inline void Pause() {
#ifndef __GNUC__
#error "GCC is needed for the macros in the `Pause()` function."
#endif
#if defined(__x86_64__)
  asm volatile("pause");
#elif defined(__aarch64__)
  asm volatile("yield");
#elif defined(__powerpc64__)
  asm volatile("or 27,27,27");
#else
  // Do nothing.
#endif
}

// This class encapsulates a GTID (ghOSt thread identifier).
//
// Example:
// Gtid gtid = Gtid::Current();
// CHECK_EQ(gtid.id(), my_gtid);
// pid_t my_tid = gtid.tid();
// ...
// int64_t some_gtid = 104'326;
// Gtid gtid(some_gtid);
// pid_t some_tid = gtid.tid();
class Gtid {
 public:
  Gtid() : gtid_raw_(-1) {}  // Uninitialized.
  explicit Gtid(int64_t gtid) : gtid_raw_(gtid) {}

  // Returns the GTID for the calling thread.
  static inline Gtid Current() {
    static thread_local int64_t gtid = GetGtid().value_or(-1);
    return Gtid(gtid);
  }

  // Returns the GTID for a given thread.
  static absl::StatusOr<Gtid> FromTid(int64_t tid);

  // Returns the raw GTID number.
  int64_t id() const { return gtid_raw_; }

  // Returns the TID (thread identifier) associated with the thread that has
  // this GTID.
  pid_t tid() const;

  // Returns the TGID (thread group identifier) associated with the thread that
  // has this GTID.
  pid_t tgid() const;

  bool operator==(const Gtid& b) const { return id() == b.id(); }
  bool operator!=(const Gtid& b) const { return id() != b.id(); }
  bool operator!() const { return id() == 0; }

  friend std::ostream& operator<<(std::ostream& os, const Gtid& gtid) {
    return os << gtid.id();
  }

  // These are just some simple debug helpers to make things more readable.
  // Assigns `name` to the GTID. This name will be returned on future calls to
  // `describe`.
  void assign_name(std::string name) const;

  // Returns the name for this GTID. If no name has been assigned to this GTID
  // via a call to `assign_name` then a name is automatically generated and
  // returned.
  absl::string_view describe() const;

 private:
  // The raw GTID number.
  int64_t gtid_raw_;
};

// Futex functions. See `man 2 futex` for a description of what a Futex is and
// how to use one. This Futex class supports any type `T` whose size is equal to
// int's size.
//
// Example:
// enum class SomeType {
//   kTypeZero,
//   kTypeOne,
//   kTypeTwo,
//   ...
//   kTypeFive,
// };
//
// Class member: std::atomic<SomeType> val_ = SomeType::kTypeZero;
//
// ...
//
// Thread 1:
// Futex::Wait(&val_, /*val=*/SomeType::kTypeZero);
// (This code causes thread 1 to sleep on the futex.)
//
// ...
//
// Thread 2:
// val_.store(SomeType::kTypeFive, std::memory_order_release);
// Futex::Wake(&val_, /*count=*/1);
// (This code wakes up thread 1.)
class Futex {
 public:
  // Wakes up threads waiting on the futex. Up to `count` threads waiting on `f`
  // are woken up. Note that the type `T` must be the same size as an int, which
  // is the size that futex supports. We use a template mainly to support enum
  // class types.
  template <class T>
  static int Wake(std::atomic<T>* f, int count) {
    static_assert(sizeof(T) == sizeof(int));
    int rc = futex(reinterpret_cast<std::atomic<int>*>(f), FUTEX_WAKE, count,
                   nullptr);
    return rc;
  }

  // Waits on the futex `f` while its value is `val`. Note that the type `T`
  // must be the same size as an int, which is the size that futex supports. We
  // use a template mainly to support enum class types.
  template <class T>
  static int Wait(std::atomic<T>* f, T val) {
    static_assert(sizeof(T) == sizeof(int));
    while (true) {
      int rc = futex(reinterpret_cast<std::atomic<int>*>(f), FUTEX_WAIT,
                     static_cast<int>(val), nullptr);
      if (rc == 0) {
        if (f->load(std::memory_order_acquire) != val) {
          return rc;
        }
        // This was a spurious wakeup, so sleep on the futex again.
      } else {
        if (errno == EAGAIN) {
          // Futex value mismatch.
          return 0;
        }
        CHECK_EQ(errno, EINTR);
      }
    }
  }

 private:
  // The futex system call. See `man 2 futex`.
  static int futex(std::atomic<int>* uaddr, int futex_op, int val,
                   const timespec* timeout) {
    return syscall(__NR_futex, uaddr, futex_op, val, timeout);
  }
};

// This class is a notification, which is essentially a trigger. Threads can
// sleep while the trigger has not been set (i.e., the notification is not yet
// notified) and will wake up once the trigger is set (i.e., the notification is
// notified). This class is similar to `absl::Notification` but contains extra
// functionality, such as the ability to reset.
//
// Notifications act as memory barriers.
//
// Example:
// Notification notification_;
// ...
// Thread 0:
// CHECK(!notification_.HasBeenNotified());
// notification_.WaitForNotification();
// (Thread 0 now sleeps).
// ...
// Thread 1:
// notification_.Notify();
class Notification {
 public:
  Notification() {}

  // Disallow copy and assign.
  Notification(const Notification&) = delete;
  Notification& operator=(const Notification&) = delete;

  ~Notification();

  // Returns true if the notification has been notified. Returns false
  // otherwise.
  bool HasBeenNotified() const {
    return notified_.load(std::memory_order_relaxed) ==
           NotifiedState::kNotified;
  }

  // Resets the notification back to the `unnotified` state.
  // Careful using this - you need to communicate to whoever will call Notify
  // *after* you reset, e.g. with another Notification. There is no way for the
  // notifier to 'notice' that this object is ready for another notification.
  void Reset() {
    notified_.store(NotifiedState::kNoWaiter, std::memory_order_relaxed);
  }

  // Notifies the notification. This causes `HasBeenNotified()` to return true
  // on future calls to it and wakes up any threads sleeping on a call to
  // `WaitForNotification()`.
  void Notify();

  // Puts the calling thread sleep until the notification is notified by a call
  // to `Notify()`.
  void WaitForNotification();

 private:
  enum class NotifiedState {
    kNoWaiter,
    kWaiter,
    kNotified,
  };

  friend std::ostream& operator<<(std::ostream& os,
                                  Notification::NotifiedState notified_state) {
    switch (notified_state) {
      case Notification::NotifiedState::kNoWaiter:
        return os << "No Waiter";
      case Notification::NotifiedState::kWaiter:
        return os << "Waiter";
      case Notification::NotifiedState::kNotified:
        return os << "Notified";
    }
  }

  // The notification state.
  std::atomic<NotifiedState> notified_ = NotifiedState::kNoWaiter;
};

// Parent and child are codependent.  If the parent dies, the child dies.  If
// the child exits with a non-zero status or is terminated by a signal, the
// parent exits with the same error (if possible).  The parent can add an exit
// handler to handle the error on its own.  A parent can wait on a child, which
// will catch the normal exit(0).
class ForkedProcess {
 public:
  // Returns to both the child and parent.  Caller needs to handle both cases.
  // stderr_fd is the FD in the caller (parent) that the child will use for
  // stderr.
  ForkedProcess(int stderr_fd = 2);
  // The child runs 'lambda' and exits.
  explicit ForkedProcess(std::function<int()> lambda);
  ForkedProcess(const ForkedProcess&) = delete;
  ForkedProcess& operator=(const ForkedProcess&) = delete;
  ~ForkedProcess();

  int WaitForChildExit();
  bool IsChild() { return child_ == 0; }
  // When a child exits abnormally, we'll call these.  The handlers are passed
  // the child's pid and the wstatus, and they return true if they 'handled' the
  // problem such that the parent should *not* exit.
  void AddExitHandler(std::function<bool(pid_t, int)> handler);
  void KillChild(int signum);

 private:
  // Looks up the child and runs any registered handlers for the child's
  // abnormal exit.  Returns whether or not a handler wants to abort our exit.
  static bool HandleAbnormalExit(pid_t child, int wait_status);
  // For a given wait status, exit if the child abnormally exited (non-zero exit
  // code or a signal).
  static void HandleIfAbnormalExit(pid_t child, int wait_status);
  // Signal handler for SIGCHLD.  Do not call this directly.
  static void HandleSigchild(int signum);

  static absl::flat_hash_map<pid_t, ForkedProcess*>& GetAllChildren() {
    static absl::flat_hash_map<pid_t, ForkedProcess*>* all_children =
        new absl::flat_hash_map<pid_t, ForkedProcess*>();

    return *all_children;
  }

  pid_t child_;
  std::vector<std::function<bool(pid_t, int)>> exit_handlers_;
  static absl::Mutex mu_;
};

template <typename T1, typename T2>
static inline T1 roundup2(T1 x, T2 y) {
  return (((x) + ((y)-1)) & (~((y)-1)));
}

}  // namespace ghost

#endif  // GHOST_LIB_BASE_H_
