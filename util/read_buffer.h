//
// Created by 14037 on 2023/7/29.
//

#ifndef LEVELDB_READ_BUFFER_H
#define LEVELDB_READ_BUFFER_H

#include <cstdlib>
#include <cstdint>
#include "leveldb/env.h"

namespace leveldb {
// A class for management ptr allocated by Read in RandomAccessFile across OS
class ReadBuffer {
 public:
  // for Read caller
  ReadBuffer();

  // for compression in ReadBlock
  ReadBuffer(char* ptr, bool aligned);

  // free ptr in diff OS, and diff data(aligned or not aligned)
  ~ReadBuffer();

  // copy will cause double free
  ReadBuffer(const ReadBuffer&) = delete;
  ReadBuffer& operator=(const ReadBuffer&) = delete;

  // for compression in ReadBlock
  ReadBuffer(ReadBuffer&& buffer) noexcept;
  ReadBuffer& operator=(ReadBuffer&& buffer) noexcept;

  // for Read in Env
  void SetPtr(char* ptr, bool aligned);

  // mmap will not allocate buffer, so, ptr is null
  // it should not be double cache in block cache
  bool PtrIsNotNull() const;

 private:
  void FreePtr();
  char* ptr_;
  bool aligned_;
};

inline bool IsAligned(uint64_t val, size_t alignment){
  return (val & (alignment - 1)) == 0;
}

inline bool IsAligned(const char* ptr, size_t alignment){
  return (reinterpret_cast<uintptr_t>(ptr) & (alignment - 1)) == 0;
}

struct DirectIOAlignData{
  uint64_t offset;
  size_t size;

  // the diff offset between allocated ptr and user ptr
  size_t user_offset;
  char* ptr;
};

// get closest alignment value before val
uint64_t GetBeforeAlignedValue(uint64_t val, size_t alignment);

// get closest alignment value after val
uint64_t GetAfterAlignedValue(uint64_t val, size_t alignment);

// new aligned block in diff os
DirectIOAlignData NewAlignedData(uint64_t offset, size_t n, size_t alignment);
}  // namespace leveldb

#endif  // LEVELDB_READ_BUFFER_H
