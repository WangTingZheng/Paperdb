//
// Created by 14037 on 2023/8/2.
//

#include "leveldb/io_saver.h"
#include "util/file_impl.h"

namespace leveldb{
IOSaver::IOSaver(Env* env){
  env_ = new SpecialEnv(env);
}

IOSaver::~IOSaver() {
  delete env_;
}

Env* IOSaver::GetEnv(){
  return env_;
}

void IOSaver::StartRead(){
  SpecialEnv* env =  reinterpret_cast<SpecialEnv*>(env_);
  env->count_random_reads_.store(true, std::memory_order_release);
  env->random_read_counter_.Reset();
}

int IOSaver::GetReadIONumber(){
  SpecialEnv* env =  reinterpret_cast<SpecialEnv*>(env_);
  return env->random_read_counter_.Read();
}
}