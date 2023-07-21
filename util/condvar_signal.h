//
// Created by WangTingZheng on 2023/7/11.
//

#ifndef LEVELDB_CONDVAR_SIGNAL_H
#define LEVELDB_CONDVAR_SIGNAL_H

#include "port/port.h"
#include "port/thread_annotations.h"
#include <atomic>

namespace leveldb {
class SCOPED_LOCKABLE CondVarSignal {
 public:
  explicit CondVarSignal(port::Mutex* mu, std::atomic<bool> *done,
                         port::CondVar* condVar) EXCLUSIVE_LOCK_FUNCTION(mu)
      :mu_(mu), done_(done), condVar_(condVar){
    // lock first to protect done_ and cv in multi thread
    this->mu_->Lock();
    this->done_->store(false, std::memory_order_release);
    *this->done_ = false;
  }

  CondVarSignal(const CondVarSignal& ) = delete;
  CondVarSignal& operator=(const CondVarSignal&) = delete;

  ~CondVarSignal()UNLOCK_FUNCTION(){
    this->done_->store(true, std::memory_order_release);
    this->condVar_->SignalAll();
    // unlock last to protect done_ and cv in multi thread
    this->mu_->Unlock();
  }

 private:
  port::Mutex* const mu_;
  port::CondVar* const condVar_;
  std::atomic<bool> *const done_;
};
}
#endif  // LEVELDB_CONDVAR_SIGNAL_H
