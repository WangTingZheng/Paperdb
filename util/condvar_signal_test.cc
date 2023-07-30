//
// Created by WangTingZheng on 2023/7/11.
// signal all thread waiting for using filterblock reader
// using RAII, similar to unique_ptr
//

#include "condvar_signal.h"
#include <gtest/gtest.h>
#include "leveldb/env.h"
namespace leveldb {
  struct Controller {
    port::Mutex* mu;
    bool* done;
    port::CondVar* cv;
  };

  void DoJob(void *arg){
    Controller* controller = static_cast<Controller*>(arg);
    CondVarSignal c(controller->mu, controller->done, controller->cv);
  }

  static void BGWork(void *arg){
      DoJob(arg);
  }

TEST(CondvarSignalTest, Base){
  Controller controller{};

  bool done = false;
  port::Mutex mutex;
  port::CondVar cv(&mutex);

  controller.mu = &mutex;
  controller.done = &done;
  controller.cv = &cv;

  Env* env = Env::Default();
  // make done true, and signal all thread waiting for cv
  env->Schedule(&BGWork, &controller);

  mutex.Lock();
  // waiting background thread done
  while (!done){
    cv.Wait();
  }
  mutex.Unlock();
}

//TODO : multi thread test
}  // namespace leveldb