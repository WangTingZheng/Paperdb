//
// Created by 14037 on 2023/8/2.
//

#ifndef LEVELDB_IO_SAVER_H
#define LEVELDB_IO_SAVER_H

#include "leveldb/export.h"
#include "leveldb/env.h"

namespace leveldb{
class LEVELDB_EXPORT IOSaver{
 public:
  IOSaver(Env* env);

  ~IOSaver();

  Env* GetEnv();
  void StartRead();
  int GetReadIONumber();

 private:
  Env* env_;
};
}

#endif  // LEVELDB_IO_SAVER_H
