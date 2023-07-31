//
// Created by 14037 on 2023/7/29.
//
#include "util/read_buffer.h"
#include "gtest/gtest.h"

namespace leveldb {

const int AlignedMallocAlignment = 1024;

char* AlignedMalloc(){
#ifdef _WIN32
  //TODO make 1024 become a variable
  return reinterpret_cast<char *>(_aligned_malloc(1024, AlignedMallocAlignment));
#else
  char *buf = nullptr;
  if(posix_memalign(reinterpret_cast<void **>(&buf), AlignedMallocAlignment,
                     1024) != 0){
    return nullptr;
  }
  return buf;
#endif
}

TEST(ReadBufferTest, Empty) {
  ReadBuffer read_buffer;
}

TEST(ReadBufferTest, Base) {
  char* ptr = (char*)malloc(sizeof(char) * 10);
  ReadBuffer read_buffer(ptr, /*aligned=*/false);
  ASSERT_TRUE(read_buffer.PtrIsNotNull());
}

TEST(ReadBufferTest, BaseAligned) {
  char* ptr = AlignedMalloc();
  ASSERT_NE(ptr, nullptr);
  ReadBuffer read_buffer(ptr, /*aligned=*/true);
  ASSERT_TRUE(read_buffer.PtrIsNotNull());
}

TEST(ReadBufferTest, Multi) {
  char* ptr1 = (char*)malloc(sizeof(char) * 10);
  ReadBuffer read_buffer(ptr1, /*aligned=*/false);
  ASSERT_TRUE(read_buffer.PtrIsNotNull());

  char* ptr2 = (char*)malloc(sizeof(char) * 10);
  read_buffer.SetPtr(ptr2, /*aligned=*/false);
  ASSERT_TRUE(read_buffer.PtrIsNotNull());
}

TEST(ReadBufferTest, MultiAligned) {
  char* ptr1 = AlignedMalloc();
  ASSERT_NE(ptr1, nullptr);
  ReadBuffer read_buffer(ptr1, /*aligned=*/true);
  ASSERT_TRUE(read_buffer.PtrIsNotNull());

  char* ptr2 = AlignedMalloc();
  ASSERT_NE(ptr2, nullptr);
  read_buffer.SetPtr(ptr2, /*aligned=*/true);
  ASSERT_TRUE(read_buffer.PtrIsNotNull());
}

TEST(ReadBufferTest, GetBeforeAlignedValue){
  size_t alignment = 1024;

  for(int i = 0; i < 1024; i++){
    ASSERT_EQ(GetBeforeAlignedValue(i, alignment), 0);
  }

  for(int i = 1024; i < 2 * 1024; i++){
    ASSERT_EQ(GetBeforeAlignedValue(i, alignment), 1024);
  }
}

TEST(ReadBufferTest, GetAfterAlignedValue){
  size_t alignment = 1024;

  for(int i = 1; i <= 1024; i++){
    ASSERT_EQ(GetAfterAlignedValue(i, alignment), 1024);
  }

  for(int i = 1024 + 1; i < 2 * 1024; i++){
    ASSERT_EQ(GetAfterAlignedValue(i, alignment), 2 * 1024);
  }
}

TEST(ReadBufferTest, NewAlignedData){
  uint64_t offset = 1025;
  size_t size = 1022;
  size_t alignment = 1024;

  DirectIOAlignData data = NewAlignedData(offset, size, alignment);
  ReadBuffer read_buffer;
  read_buffer.SetPtr(data.ptr, /*aligned=*/true);

  ASSERT_EQ(data.offset, 1024);
  ASSERT_TRUE(IsAligned(data.ptr, alignment));
  ASSERT_EQ(data.user_offset, 1);
  ASSERT_EQ(data.size, 1024);
}
}