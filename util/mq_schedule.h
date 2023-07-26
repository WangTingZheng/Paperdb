//
// Created by WangTingZheng on 2023/7/10.
//

#ifndef LEVELDB_MQ_SCHEDULE_H
#define LEVELDB_MQ_SCHEDULE_H

#include <atomic>
#include <queue>

#include "leveldb/multi_queue.h"

#include "port/port.h"

#include "mutexlock.h"
namespace leveldb {

struct BackgroundWorkItem {
  explicit BackgroundWorkItem(void (*function)(void*), void* arg)
      : function(function), arg(arg) {}

  void (*const function)(void*);
  void* const arg;
};

class MQScheduler {
 public:
  MQScheduler(): background_work_cv_(&background_work_mutex_), background_finished_cv_(&background_work_mutex_),
                 started_background_thread_(false), shutting_down_(true){}
  void Schedule(void (*background_work_function)(void*),
             void* background_work_arg);

  static MQScheduler* Default();

  void ShutDown(){
    MutexLock l(&background_work_mutex_);
    while (!shutting_down_){
      background_finished_cv_.Wait();
    }
  }

 private:
  port::Mutex background_work_mutex_;
  port::CondVar background_work_cv_ GUARDED_BY(background_work_mutex_);
  port::CondVar background_finished_cv_ GUARDED_BY(background_work_mutex_);
  bool started_background_thread_ GUARDED_BY(background_work_mutex_);
  bool shutting_down_;

  std::queue<BackgroundWorkItem> background_work_queue_
      GUARDED_BY(background_work_mutex_);

  void BackgroundThreadMain();
  static void BackgroundThreadEntryPoint(MQScheduler* env){
    env->BackgroundThreadMain();
  }
};
}  // namespace leveldb

#endif  // LEVELDB_MQ_SCHEDULE_H
