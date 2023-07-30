//
// Created by WangTingZheng on 2023/7/10.
//
#include "mq_schedule.h"

#include <gtest/gtest.h>

#include "leveldb/env.h"

#include "mutexlock.h"

namespace leveldb{

class MQScheduleTest : public testing::Test {
 public:
  MQScheduleTest():env_(MQScheduler::Default()){}

  MQScheduler* env_;
};

TEST_F(MQScheduleTest, RunImmediately) {
  struct RunState {
    port::Mutex mu;
    port::CondVar cvar{&mu};
    bool called = false;

    static void Run(void* arg) {
      RunState* state = reinterpret_cast<RunState*>(arg);
      MutexLock l(&state->mu);
      ASSERT_EQ(state->called, false);
      state->called = true;
      state->cvar.Signal();
    }
  };

  RunState state;
  MQScheduler mqs;

  env_->Schedule(&RunState::Run, &state);

  MutexLock l(&state.mu);
  while (!state.called) {
    state.cvar.Wait();
  }
}

struct Manager {
  port::Mutex *mutex;
  MQScheduler* env;
  bool* done;
  port::CondVar* cv;

  bool* compaction_done;
  port::CondVar* compaction_cv;
  port::Mutex *compaction_mutex;
};

static void LoadFilter(void* arg) {
  Manager* manager = static_cast<Manager*>(arg);
  // wake up compaction thread to use filer
  manager->mutex->Lock(); // protect done and cv
  *manager->done = true;
  manager->cv->SignalAll();
  manager->mutex->Unlock();
}

static void Compaction(void* arg) {
  Manager* manager = static_cast<Manager*>(arg);
  manager->env->SetSignal(manager->done, manager->mutex, manager->cv);
  // deadlock here
  // thread in schedule is working for compaction
  // just running "background_work_function(background_work_arg);" in BackgroundThreadMain
  // finished after LoadingFilter done
  manager->env->Schedule(LoadFilter, manager);

  // protect done and cv
  manager->mutex->Lock();
  //waiting for using reader
  while (!*(manager->done)) {
    manager->cv->Wait();
  }
  manager->mutex->Unlock();
}

TEST_F(MQScheduleTest, DeadlockInMQ) {
  Manager manager;
  bool done = false;
  port::Mutex mutex;
  port::CondVar cv(&mutex);

  MQScheduler* mq_env = MQScheduler::Default();
  manager.mutex = &mutex;
  manager.done = &done;
  manager.env = mq_env;
  manager.cv = &cv;

  port::Mutex compaction_mutex;
  compaction_mutex.Lock();
  bool compaction_done = false;
  port::CondVar compaction_cv(&compaction_mutex);
  compaction_mutex.Unlock();

  manager.compaction_done = &compaction_done;
  manager.compaction_cv = &compaction_cv;
  manager.compaction_mutex = &compaction_mutex;

  Env* env = Env::Default();
  env->Schedule(Compaction, &manager);

  compaction_mutex.Lock();
  // compaction thread never wake up
  while (!compaction_done) {
    compaction_cv.Wait();
  }
  compaction_mutex.Unlock();
}
}