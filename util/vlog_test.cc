//
// Created by WangTingZheng on 2023/6/27.
//
#include "leveldb/filter_policy.h"

#include "util/coding.h"

#include "arena.h"
#include "gtest/gtest.h"
#include "vlog_reader.h"
#include "vlog_writer.h"

namespace leveldb {
class StringSink : public WritableFile {
 public:
  ~StringSink() override = default;

  const std::string& contents() const { return contents_; }

  Status Close() override { return Status::OK(); }
  Status Flush() override { return Status::OK(); }
  Status Sync() override { return Status::OK(); }

  Status Append(const Slice& data) override {
    contents_.append(data.data(), data.size());
    return Status::OK();
  }

 private:
  std::string contents_;
};

class StringSource : public RandomAccessFile {
 public:
  StringSource(const Slice& contents)
      : contents_(contents.data(), contents.size()) {}

  ~StringSource() override = default;

  uint64_t Size() const { return contents_.size(); }

  Status Read(uint64_t offset, size_t n, Slice* result,
              char* scratch) const override {
    if (offset >= contents_.size()) {
      return Status::InvalidArgument("invalid Read offset");
    }
    if (offset + n > contents_.size()) {
      n = contents_.size() - offset;
    }
    std::memcpy(scratch, &contents_[offset], n);
    *result = Slice(scratch, n);
    return Status::OK();
  }

 private:
  std::string contents_;
};

class VlogTest : public testing::Test {
 public:
  VlogTest() : reader_(nullptr), source_(nullptr), sink_(new StringSink()) {
    writer_ = new VlogWriter(sink_, 0);
  }

  void Add(Slice key, Slice value, std::string* handle) {
    writer_->Add(key, value, handle);
  }

  void FinishAdd() {
    if (reader_ == nullptr) {
      source_ = new StringSource(sink_->contents());
      reader_ = new VlogReader(source_);
    }
  }

  void Reader(Slice* value, std::string* handle) {
    FinishAdd();
    uint64_t entry_size = 0;
    Slice handle_slice = Slice(handle->data(), handle->size());
    VlogReader::GetEntrySize(handle_slice, &entry_size);

    char* buf = arena.Allocate(entry_size);
    reader_->ReadRecond(Slice(handle->data(), handle->size()), value, buf,
                        entry_size);
  }

  ~VlogTest() override {
    delete writer_;
    delete reader_;
    delete source_;
    delete sink_;
  }

 private:
  VlogWriter* writer_;
  VlogReader* reader_;

  StringSource* source_;
  StringSink* sink_;

  Arena arena;
};

class VlogTestInFS : public testing::Test {
 public:
  VlogTestInFS() : reader_(nullptr), source_(nullptr),sink_(nullptr) {
    // get test dir path, windows has it's own file path kind
    std::string default_db_path;
    Env::Default()->GetTestDirectory(&default_db_path);
    dir_path_ = default_db_path + "/vlogtestinfs";
    file_path_ = dir_path_ + "/test_file";

    // create dir if not exists
    if(!Env::Default()->FileExists(dir_path_)){
      status_ = Env::Default()->CreateDir(dir_path_);
      if(!status_.ok()){
        return ;
      }
    }

    // create files
    status_ = Env::Default()->NewWritableFile(file_path_, &sink_);
    if(!status_.ok()){
      return ;
    }
    writer_ = new VlogWriter(sink_, 0);
  }

  void Add(Slice key, Slice value, std::string* handle) {
    writer_->Add(key, value, handle);
  }

  void FinishAdd() {
    if (reader_ == nullptr) {
      // sync and unlink file
      // open by reader
      sink_->Sync();
      delete sink_;
      sink_ = nullptr;

      status_ = Env::Default()->NewRandomAccessFile(file_path_, &source_);
      if(!status_.ok()){
        return;
      }
      reader_ = new VlogReader(source_);
    }
  }

  void Reader(Slice* value, std::string* handle) {
      FinishAdd();

      // get file size to allocate buf to hold entry data
      uint64_t entry_size = 0;
      Slice handle_slice = Slice(handle->data(), handle->size());
      VlogReader::GetEntrySize(handle_slice, &entry_size);

      // buf will be managed by arena
      char* buf = arena.Allocate(entry_size);
      reader_->ReadRecond(Slice(handle->data(), handle->size()), value, buf,
                          entry_size);
  }

  Status status() const {
    return status_;
  }

  ~VlogTestInFS() override{
    // clean file
    if(Env::Default()->FileExists(file_path_)) {
      Env::Default()->RemoveDir(dir_path_);
    }
    delete writer_;
    delete reader_;
    delete source_;
    delete sink_;
  }

 private:
  VlogWriter* writer_;
  VlogReader* reader_;

  Status status_;
  std::string dir_path_;
  std::string file_path_;

  RandomAccessFile* source_;
  WritableFile* sink_;

  Arena arena;
};

TEST_F(VlogTest, Single) {
  std::string handle;
  Add("key", "value", &handle);

  FinishAdd();

  Slice value;
  Reader(&value, &handle);
  ASSERT_EQ(value.ToString(), "value");
}

TEST_F(VlogTest, Multi) {
  std::vector<std::string> handles;
  int N = 1000;
  for (int i = 0; i < N; i++) {
    std::string handle;
    Add("key" + std::to_string(i), "value" + std::to_string(i), &handle);
    handles.push_back(handle);
  }

  FinishAdd();

  for (int i = 0; i < N; i++) {
    Slice value;
    std::string handle = handles[i];
    Reader(&value, &handle);
    ASSERT_EQ(value.ToString(), "value" + std::to_string(i));
  }
}

TEST_F(VlogTestInFS, Single) {
  ASSERT_TRUE(Status().ok());

  std::string handle;
  Add("key", "value", &handle);

  FinishAdd();
  ASSERT_TRUE(Status().ok());

  Slice value;
  Reader(&value, &handle);
  ASSERT_EQ(value.ToString(), "value");
}

TEST_F(VlogTestInFS, Multi) {
  ASSERT_TRUE(Status().ok());

  std::vector<std::string> handles;
  int N = 1000;
  for (int i = 0; i < N; i++) {
    std::string handle;
    Add("key" + std::to_string(i), "value" + std::to_string(i), &handle);
    handles.push_back(handle);
  }

  FinishAdd();
  ASSERT_TRUE(Status().ok());

  for (int i = 0; i < N; i++) {
    Slice value;
    std::string handle = handles[i];
    Reader(&value, &handle);
    ASSERT_EQ(value.ToString(), "value" + std::to_string(i));
  }
}
}  // namespace leveldb