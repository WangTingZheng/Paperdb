//
// Created by 14037 on 2023/7/29.
//
#include "read_buffer.h"
namespace leveldb{

// Allocate aligned for DirectIO
// support for posix os(Linux and macos), and Windows
// Check memory head if is aligned in Debug mode
char* NewAlignedBuffer(size_t size, size_t alignment){
#ifdef _WIN32 //64-bit/32-bit Windows
  char *buf = reinterpret_cast<char *>(_aligned_malloc(size, alignment));
  assert(IsAligned(buf, alignment));
  return buf;
#else
  char *buf = nullptr;
  if(posix_memalign(reinterpret_cast<void **>(&buf), alignment, size) != 0){
    return nullptr;
  }

  assert(IsAligned(buf, alignment));
  return buf;
#endif
}

void FreeAlignedBuffer(char* ptr){
#ifdef _WIN32 //64-bit/32-bit Windows
  _aligned_free(ptr);
#else // for linux and macos
  free(ptr);
#endif
}

ReadBuffer::ReadBuffer()
    :ptr_(nullptr),aligned_(false){}

ReadBuffer::ReadBuffer(char* ptr, bool aligned)
    :ptr_(ptr),aligned_(aligned){}

ReadBuffer::ReadBuffer(ReadBuffer&& buffer) noexcept {
  if(this != &buffer) {
    // move ptr from buffer and clear it
    this->ptr_ = buffer.ptr_ ;
    this->aligned_ = buffer.aligned_;
    buffer.ptr_ = nullptr;
  }
}

ReadBuffer& ReadBuffer::operator=(ReadBuffer&& buffer) noexcept {
  // Todo: why?
  if(this != &buffer) {
    this->ptr_ = buffer.ptr_;
    this->aligned_ = buffer.aligned_;
    buffer.ptr_ = nullptr;
  }
  return *this;
}

void ReadBuffer::SetPtr(char* ptr, bool aligned){
  FreePtr(); // free before update
  ptr_ = ptr;
  aligned_ = aligned;
}

bool ReadBuffer::PtrIsNotNull() const{
  return ptr_ != nullptr;
}

void ReadBuffer::FreePtr(){
  if(ptr_ == nullptr) return;
  if(!aligned_){
    free(ptr_);
  }else {
    FreeAlignedBuffer(ptr_);
  }

  ptr_ = nullptr;
}

ReadBuffer::~ReadBuffer(){
  FreePtr();
}

uint64_t GetBeforeAlignedValue(uint64_t val, size_t alignment){
  // val & (alignment - 1) means val % alignment
  return val - (val & (alignment - 1));
}

uint64_t GetAfterAlignedValue(uint64_t val, size_t alignment){
  size_t mod = (val & (alignment - 1));
  // move 0 if val is aligned at the beginning to save memory
  size_t slop = (mod == 0?0:(alignment - mod));
  return val + slop;
}

// Make [offset, size] aligned according alignment
// Use by Read in DirectIORandomAccessFile
// Allocate aligned char buffer for save reading data
DirectIOAlignData NewAlignedData(uint64_t offset, size_t n, size_t alignment){
  uint64_t aligned_offset = GetBeforeAlignedValue(offset, alignment);

  // update size, ready to align
  size_t user_data_offset = offset - aligned_offset;
  size_t aligned_size = n + user_data_offset;

  // align size at last
  aligned_size = GetAfterAlignedValue(aligned_size, alignment);

  // check new offset and size if is aligned in Debug mod
  assert(IsAligned(aligned_offset, alignment));
  assert(IsAligned(aligned_size, alignment));

  // save val in struct
  DirectIOAlignData data{};
  data.offset = aligned_offset;
  data.size   = aligned_size;
  data.user_offset  = user_data_offset;

  // new aligned buffer according to aligned size
  data.ptr = NewAlignedBuffer(aligned_size, alignment);
  assert(IsAligned(data.ptr, alignment));

  return data;
}
};