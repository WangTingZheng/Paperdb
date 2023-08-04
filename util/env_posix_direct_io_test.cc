//
// Created by 14037 on 2023/8/3.
//
#include "util/testutil.h"

#include "env_posix_test_helper.h"
#include "read_buffer.h"
#include <cinttypes>

namespace leveldb {

// 1 for buffer io, 1 for direct io
static const int kReadOnlyFileLimit = 2;

// buffer io can be used only we close mmap
static const int kMMapLimit = 0;

static const int kSectionSize = 512;

static const int kTableSize = 4 * 1024 * 1024;

class EnvPosixDirectIOTest : public testing::Test {
 public:
  static void SetFileLimits(int read_only_file_limit, int mmap_limit) {
    EnvPosixTestHelper::SetReadOnlyFDLimit(read_only_file_limit);
    EnvPosixTestHelper::SetReadOnlyMMapLimit(mmap_limit);
  }

  EnvPosixDirectIOTest() : env_(Env::Default()) {}

  // Read 1 section(512Byte) data 1000 times
  Status GetReadTime(RandomAccessFile* file, uint64_t& read_time) const{
    uint64_t times = 1000;
    Status s;
    uint64_t now = env_->NowMicros();
    for(int i = 0; i < times; i ++) {
      ReadBuffer read_buffer;
      Slice result;
      s = file->Read(0, kSectionSize, &result, &read_buffer);
      if(!s.ok()){
        return s;
      }
      if(result.size() != kSectionSize){
        return Status::Corruption("read data size is wrong");
      }
    }
    uint64_t done = env_->NowMicros();
    read_time = done - now;
    return Status::OK();
  }

  Status GetBufferAndDirectReadTime(const std::string& file_path,
                                    uint64_t& buffer_io_time,
                                    uint64_t& direct_io_time) const{
    // get buffer io read time
    leveldb::RandomAccessFile* buffer_io_file = nullptr;
    Status s;
    s = env_->NewRandomAccessFile(file_path, &buffer_io_file);
    if(!s.ok()){
      return s;
    }
    s = GetReadTime(buffer_io_file, buffer_io_time);
    if(!s.ok()){
      return s;
    }

    // get direct io read time
    leveldb::RandomAccessFile* direct_io_file = nullptr;
    s = env_->NewDirectIORandomAccessFile(file_path, &direct_io_file);
    if(!s.ok()){
      return s;
    }
    s = GetReadTime(direct_io_file, direct_io_time);
    if(!s.ok()){
      return s;
    }

    delete buffer_io_file;
    delete direct_io_file;

    return s;
  }

  Status WriteData(const std::string& file_path, int file_size) const{
    // write data to disk
    FILE* f = std::fopen(file_path.c_str(), "we");
    if(f == nullptr){
      return Status::IOError("fopen failed");
    }

    std::string data;
    data.reserve(file_size);
    for(int i = 0; i < file_size; i++){
      data += '0';
    }

    fputs(data.c_str(), f);
    std::fclose(f);

    // check file size
    uint64_t real_file_size = 0;
    Status s = env_->GetFileSize(file_path, &real_file_size);
    if(!s.ok()){
      return s;
    }
    if(file_size != real_file_size){
      return Status::Corruption("file size not match");
    }

    return Status::OK();
  }

  Env* env_;
};

TEST_F(EnvPosixDirectIOTest, TestBufferIOAndDirectIO){
#ifdef __APPLE__
  GTEST_SKIP() << "skipping direct io test";
#endif

  uint64_t buffer_io_time = 0;
  uint64_t direct_io_time = 0;

  std::string test_dir;
  ASSERT_LEVELDB_OK(env_->GetTestDirectory(&test_dir));
  std::string file_path = test_dir + "/buffer_io_and_direct_io.txt";

  ASSERT_TRUE(WriteData(file_path, kTableSize).ok());
  ASSERT_TRUE(GetBufferAndDirectReadTime(file_path, buffer_io_time,
                                         direct_io_time).ok());

  // to fix macos
  fprintf(stderr, "buffer io %" PRIu64 ", direct io %" PRIu64 "\n",
          buffer_io_time, direct_io_time);

  ASSERT_LE(buffer_io_time, direct_io_time);
  ASSERT_LE(buffer_io_time * 10, direct_io_time);

  ASSERT_LEVELDB_OK(env_->RemoveFile(file_path));
}

TEST_F(EnvPosixDirectIOTest, TestBufferIOAndDirectIOBigFile){
#ifdef __APPLE__
  GTEST_SKIP() << "skipping direct io test";
#endif

  uint64_t buffer_io_time = 0;
  uint64_t direct_io_time = 0;

  std::string test_dir;
  ASSERT_LEVELDB_OK(env_->GetTestDirectory(&test_dir));
  std::string file_path = test_dir + "/buffer_io_and_direct_io_big_file.txt";

  ASSERT_TRUE(WriteData(file_path, kTableSize * 100).ok());
  ASSERT_TRUE(GetBufferAndDirectReadTime(file_path, buffer_io_time,
                                         direct_io_time).ok());

  fprintf(stderr, "buffer io %" PRIu64 ", direct io %" PRIu64 "\n",
          buffer_io_time, direct_io_time);

  ASSERT_LE(buffer_io_time     , direct_io_time);
  ASSERT_LE(buffer_io_time * 10, direct_io_time);

  ASSERT_LEVELDB_OK(env_->RemoveFile(file_path));
}
}
int main(int argc, char** argv) {
  // All tests currently run with the same read-only file limits.
  leveldb::EnvPosixDirectIOTest::SetFileLimits(leveldb::kReadOnlyFileLimit,
                                               leveldb::kMMapLimit);

  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}