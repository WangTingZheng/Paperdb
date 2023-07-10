//
// Created by WangTingZheng on 2023/7/10.
//

#ifndef LEVELDB_MQ_SCHEDULE_H
#define LEVELDB_MQ_SCHEDULE_H

#include <queue>

#include "port/port.h"
namespace leveldb {

struct BackgroundWorkItem {
  explicit BackgroundWorkItem(void (*function)(void* arg), void* arg)
      : function(function), arg(arg) {}

  void (*const function)(void*);
  void* const arg;
};

class MQSchedule {
 public:
  MQSchedule(): background_work_cv_(&background_work_mutex_),
                 started_background_thread_(false){}
  void Schedule(void (*background_work_function)(void* background_work_arg),
    void* background_work_arg);
  void BackgroundThreadMain();
  static MQSchedule* Default();
  static void BackgroundThreadEntryPoint(MQSchedule* env){
    env->BackgroundThreadMain();
  }
 private:
  port::Mutex background_work_mutex_;
  port::CondVar background_work_cv_ GUARDED_BY(background_work_mutex_);
  bool started_background_thread_ GUARDED_BY(background_work_mutex_);

  std::queue<BackgroundWorkItem> background_work_queue_
      GUARDED_BY(background_work_mutex_);
};

}  // namespace leveldb

#endif  // LEVELDB_MQ_SCHEDULE_H