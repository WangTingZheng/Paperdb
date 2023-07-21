//
// Created by WangTingZheng on 2023/7/10.
// Just copy from util/env_posix.cc for multi queue background thread
// Call env->Schedule in env->Schedule in Compaction thread will cause deadlock
// take part of Schedule from Env to multi queue
//

#include "mq_schedule.h"
#include <atomic>
#include <thread>


namespace leveldb {

void MQScheduler::BackgroundThreadMain() {
  while (true) {
    background_work_mutex_.Lock();

    // Wait until there is work to be done.
    while (background_work_queue_.empty()) {
      background_work_cv_.Wait();
    }

    assert(!background_work_queue_.empty());
    auto background_work_function = background_work_queue_.front().function;
    void* background_work_arg = background_work_queue_.front().arg;
    background_work_queue_.pop();

    background_work_mutex_.Unlock();
    background_work_function(background_work_arg);
  }
}

void MQScheduler::Schedule(void (*background_work_function)(void*),
                          void* background_work_arg) {
  background_work_mutex_.Lock();
  // Start the background thread, if we haven't done so already.
  if (!started_background_thread_) {
    started_background_thread_ = true;
    std::thread background_thread(BackgroundThreadEntryPoint, this);
    background_thread.detach();
  }

  // If the queue is empty, the background thread may be waiting for work.
  if (background_work_queue_.empty()) {
    background_work_cv_.Signal();
  }

  background_work_queue_.emplace(background_work_function, background_work_arg);
  background_work_mutex_.Unlock();
}
namespace {

// Wraps an Env instance whose destructor is never created.
//
// Intended usage:
//   using PlatformSingletonEnv = SingletonEnv<PlatformEnv>;
//   void ConfigurePosixEnv(int param) {
//     PlatformSingletonEnv::AssertEnvNotInitialized();
//     // set global configuration flags.
//   }
//   Env* Env::Default() {
//     static PlatformSingletonEnv default_env;
//     return default_env.env();
//   }
template <typename EnvType>
class SingletonScheduler {
 public:
  SingletonScheduler() {
#if !defined(NDEBUG)
    env_initialized_.store(true, std::memory_order_relaxed);
#endif  // !defined(NDEBUG)
    static_assert(sizeof(env_storage_) >= sizeof(EnvType),
                  "env_storage_ will not fit the Env");
    static_assert(alignof(decltype(env_storage_)) >= alignof(EnvType),
                  "env_storage_ does not meet the Env's alignment needs");
    new (&env_storage_) EnvType();
  }
  ~SingletonScheduler() = default;

  SingletonScheduler(const SingletonScheduler&) = delete;
  SingletonScheduler& operator=(const SingletonScheduler&) = delete;

  MQScheduler* env() { return reinterpret_cast<MQScheduler*>(&env_storage_); }

 private:
  typename std::aligned_storage<sizeof(EnvType), alignof(EnvType)>::type
      env_storage_;
#if !defined(NDEBUG)
  static std::atomic<bool> env_initialized_;
#endif  // !defined(NDEBUG)
};

#if !defined(NDEBUG)
template <typename EnvType>
std::atomic<bool> SingletonScheduler<EnvType>::env_initialized_;
#endif  // !defined(NDEBUG)

using DefaultSchedule = SingletonScheduler<MQScheduler>;

}  // namespace

// create singleton no destruction object
MQScheduler* MQScheduler::Default() {
  static DefaultSchedule env_container;
  return env_container.env();
}
}  // namespace leveldb