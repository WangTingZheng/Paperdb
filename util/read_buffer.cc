//
// Created by 14037 on 2023/7/29.
//
#include "read_buffer.h"
namespace leveldb{

ReadBuffer::ReadBuffer()
    :ptr_(nullptr),aligned_(false){}

ReadBuffer::ReadBuffer(char* ptr, bool aligned)
    :ptr_(ptr),aligned_(aligned){}

ReadBuffer::ReadBuffer(ReadBuffer&& buffer) noexcept {
  if(this != &buffer) {
    this->ptr_ = buffer.ptr_;
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
  FreePtr();
  ptr_ = ptr;
  aligned_ = aligned;
}

bool ReadBuffer::PtrIsNotNull() const{
  return ptr_ == nullptr;
}

void ReadBuffer::FreePtr(){
  if(ptr_ == nullptr) return;
  if(!aligned_){
    free(ptr_);
  }else {
#ifdef _WIN32 //64-bit/32-bit Windows
    _aligned_free(ptr_);
#else
    free(ptr_);
#endif
  }

  ptr_ = nullptr;
}

ReadBuffer::~ReadBuffer(){
  FreePtr();
}

uint64_t GetBeforeAlignedValue(uint64_t val, size_t alignment){
  return val - (val & (alignment - 1));
}

uint64_t GetAfterAlignedValue(uint64_t val, size_t alignment){
  size_t mod = (val & (alignment - 1));
  size_t slop = (mod == 0?0:(alignment - mod));
  return val + slop;
}

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

DirectIOAlignData NewAlignedData(uint64_t offset, size_t n, size_t alignment){
  uint64_t aligned_offset = GetBeforeAlignedValue(offset, alignment);

  // update size, ready to align
  size_t user_data_offset = offset - aligned_offset;
  size_t aligned_size = n + user_data_offset;

  // align size at last
  aligned_size = GetAfterAlignedValue(aligned_size, alignment);

  assert(IsAligned(aligned_offset, alignment));
  assert(IsAligned(aligned_size, alignment));

  DirectIOAlignData data{};
  data.offset = aligned_offset;
  data.size   = aligned_size;
  data.user_offset  = user_data_offset;

  data.ptr = NewAlignedBuffer(aligned_size, alignment);
  assert(IsAligned(data.ptr, alignment));
  return data;
}

};