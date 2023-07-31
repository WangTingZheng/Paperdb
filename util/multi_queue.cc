#include "leveldb/multi_queue.h"

#include <unordered_map>
#include "mq_schedule.h"
#include "mutexlock.h"

namespace leveldb {
MultiQueue::~MultiQueue() {}
struct QueueHandle {
  FilterBlockReader* reader;
  void (*deleter)(const Slice&, FilterBlockReader* reader);
  QueueHandle* next;
  QueueHandle* prev;

  bool internal_node;

  size_t key_length;
  char key_data[1];  // Beginning of key

  Slice key() const {
    // internal node for linked list has no key
    // maybe cause some fault to use internal node's key
    assert(!internal_node);

    return Slice(key_data, key_length);
  }
};

class SingleQueue {
 public:
  SingleQueue() {
    mru_ = new QueueHandle();
    lru_ = new QueueHandle();

    // mark as internal node
    // do not call Key()
    mru_->internal_node = true;
    lru_->internal_node = true;

    mru_->next = lru_;
    mru_->prev = nullptr;

    lru_->prev = mru_;
    lru_->next = nullptr;
  }

  ~SingleQueue() {
    // user node created by malloc
    QueueHandle* next = nullptr;
    for (QueueHandle* e = mru_->next; e != nullptr && e != lru_ && e != mru_;e = next) {
      next = e->next;
      FreeHandle(e);
    }

    // internal node created by new
    delete mru_;
    delete lru_;
  }

  QueueHandle* Insert(const Slice& key, FilterBlockReader* reader,
                      void (*deleter)(const Slice&, FilterBlockReader*)) {
    QueueHandle* e =  // -1 for first char in key
        reinterpret_cast<QueueHandle*>(
            malloc(sizeof(QueueHandle) - 1 + key.size()));
    e->reader = reader;
    e->deleter = deleter;
    e->key_length = key.size();
    // mark as non internal node
    // key() can be called
    e->internal_node = false;
    std::memcpy(e->key_data, key.data(), key.size());
    Queue_Append(e);
    return e;
  }

  void Erase(QueueHandle* handle){
    if (handle != nullptr) {
      Queue_Remove(handle);
      FreeHandle(handle);
    }
  }

  void Remove(QueueHandle* handle) {
    Queue_Remove(handle);
  }

  void Append(QueueHandle* handle){
    Queue_Append(handle);
  }

  void MoveToMRU(QueueHandle* handle) {
    Queue_Remove(handle);
    Queue_Append(handle);
  }

  void FindColdFilter(uint64_t& memory, SequenceNumber sn,
                      std::vector<QueueHandle*>& filters) {
    // search from LRU to MRU
    QueueHandle* e = lru_->prev;
    do {
      // do not access internal node
      if (e == nullptr || e == mru_ || e == lru_) {
        break;
      }
      if (e->reader->IsCold(sn)) {
        memory -= e->reader->OneUnitSize();
        filters.emplace_back(e);
      } else {
        break ;
      }
      e = e->prev;
    } while (memory > 0); // evict cold memory can be bigger than load a hot filter
  }

 private:
  QueueHandle *mru_; // head
  QueueHandle *lru_;

  void FreeHandle(QueueHandle* handle){
    if(handle) {
      // free reader, and free node
      handle->deleter(handle->key(), handle->reader);
      free(handle);
    }
  }

  void Queue_Append(QueueHandle* e){
    // Make "e" newest entry by inserting just after *mru
    // set e
    e->prev = mru_;
    e->next = mru_->next;

    // set node in linked list
    mru_->next->prev = e;
    mru_->next = e;
  }

  void Queue_Remove(QueueHandle* e){
    e->next->prev = e->prev;
    e->prev->next = e->next;
  }
};

class InternalMultiQueue : public MultiQueue {
 public:
  explicit InternalMultiQueue() : usage_(0),
                                  logger_(nullptr),
                                  adjustment_time_(0),
                                  scheduler(MQScheduler::Default()){
    queues_.resize(filters_number + 1);
    for (int i = 0; i < filters_number + 1; i++) {
      queues_[i] = new SingleQueue();
    }
  }

  ~InternalMultiQueue() override {
    // save adjustment times when db is over by force
    shutting_down_mutex_.Lock();
    scheduler->ShutDown();
    shutting_down_mutex_.Unlock();
    MutexLock l(&mutex_);
    Log(logger_, "Adjustment: Apply %zu times until now",
        adjustment_time_.load(std::memory_order_acquire));
#ifdef DEBUG
    // check usage if same as map
    uint64_t real_usage = 0;
    for(auto & iter : map_){
      real_usage += iter.second->reader->Size();
    }
    assert(real_usage == usage_);
#endif
    for (int i = 0; i < filters_number + 1; i++) {
      delete queues_[i];
      queues_[i] = nullptr;
    }
  }

  static void LoadFilterBGWork(void *arg){
    FilterBlockReader* reader = static_cast<FilterBlockReader*>(arg);
    reader->InitLoadFilter();
  }

  Handle* Insert(const Slice& key, FilterBlockReader* reader,
                 void (*deleter)(const Slice&, FilterBlockReader*)) override {
    MutexLock l(&mutex_);
    if(reader == nullptr) return nullptr;
    // insert to queue

    size_t number = reader->LoadFilterNumber();
    SingleQueue* queue = queues_[number];
    QueueHandle* handle = queue->Insert(key, reader, deleter);

    // insert to hashtable
    map_.emplace(key.ToString(), handle);

    // update usage
    usage_ += reader->LoadFilterNumber() * reader->OneUnitSize();

    scheduler->Schedule(LoadFilterBGWork, reader);

    return reinterpret_cast<Handle*>(handle);
  }

  void DoAdjustment(Handle* handle, const SequenceNumber& sn) override{
    MutexLock l(&mutex_);
    QueueHandle* queue_handle = reinterpret_cast<QueueHandle*>(handle);
    SingleQueue* single_queue = FindQueue(queue_handle);
    if(single_queue){
      // change to hot handle
      single_queue->MoveToMRU(queue_handle);
      Adjustment(queue_handle, sn);
    }
  }

  // todo: use reference maybe stuck?
  static inline bool ParseKey(const Slice& key, SequenceNumber* sn){
      const size_t n = key.size();
      if (n < 8) return false;
      uint64_t num = DecodeFixed64(key.data() + n - 8);
      *sn = num >> 8;
      return ((num & 0xff) <= static_cast<uint8_t>(kTypeValue));
  }

  bool UpdateHandle(Handle* handle, uint64_t block_offset, const Slice& key) override{
    // try to apply adjustment
    SequenceNumber sn;
    if (ParseKey(key, &sn)) {
#ifdef DEBUG
      ParsedInternalKey internal_key;
      assert(ParseInternalKey(key, &internal_key));
      assert(internal_key.sequence == sn);
#endif
      FilterBlockReader* reader = Value(handle);
      if (reader) {
        // update access time first
        reader->UpdateState(sn);

        // adjust if reader is hot
        DoAdjustment(handle, sn);

        return reader->KeyMayMatch(block_offset, key);
      }
    }
    return true;
  }

  Handle* Lookup(const Slice& key) override {
    MutexLock l(&mutex_);
    auto iter = map_.find(key.ToString());
    if (iter != map_.end()) {
      QueueHandle* handle = iter->second;
      return reinterpret_cast<Handle*>(handle);
    } else {
      return nullptr;
    }
  }

  void GoBackToInitFilter(Handle* handle, RandomAccessFile* file) override {
    MutexLock l(&mutex_);
    if(handle != nullptr){
      QueueHandle* queue_handle = reinterpret_cast<QueueHandle*>(handle);
      FilterBlockReader* reader = queue_handle->reader;
      size_t filter_number = reader->FilterUnitsNumber();
      size_t init_filter_number  = reader->LoadFilterNumber();

      queues_[filter_number]->Remove(queue_handle);
      queues_[init_filter_number]->Append(queue_handle);

      reader->GoBackToInitFilter(file);
      usage_ += (init_filter_number - filter_number) * reader->OneUnitSize();
    }
  }

  FilterBlockReader* Value(Handle* handle) override {
    if (handle) {
      return reinterpret_cast<QueueHandle*>(handle)->reader;
    } else {
      return nullptr;
    }
  }

  void Release(Handle* handle) override {
    MutexLock l(&mutex_);
    if (handle != nullptr) {
      QueueHandle* queue_handle = reinterpret_cast<QueueHandle*>(handle);
      Status s;
      while(queue_handle->reader->CanBeEvict()) {
        s = EvictHandle(queue_handle);
        assert(s.ok());
        usage_ -= queue_handle->reader->OneUnitSize();
      }
    }
  }

  void Erase(const Slice& key) override {
    MutexLock l(&mutex_);
    auto iter = map_.find(key.ToString());
    if (iter != map_.end()) {
      QueueHandle* queue_handle = iter->second;
      SingleQueue* queue = FindQueue(queue_handle);
      if (queue != nullptr) {
        usage_ -= queue_handle->reader->Size();
        queue->Erase(queue_handle);
      }
      map_.erase(key.ToString());
    }
  }

  size_t TotalCharge() const override {
    MutexLock l(&mutex_);
    return usage_;
  }

  // logger for record adjustment information
  void SetLogger(Logger* logger) override {
    MutexLock l(&mutex_);
    logger_ = logger;
  }

 private:
  size_t usage_ GUARDED_BY(mutex_);
  Logger* logger_ GUARDED_BY(mutex_);
  // 2^32-1 at least (42,9496,7295)
  // 2^64-1 at most  (1844,6744,0737,0955,1615)
  std::atomic<size_t> adjustment_time_;

  mutable port::Mutex mutex_;
  mutable port::Mutex shutting_down_mutex_;

  std::vector<SingleQueue*> queues_ GUARDED_BY(mutex_);
  std::unordered_map<std::string, QueueHandle*> map_ GUARDED_BY(mutex_);

  MQScheduler* scheduler;

  std::vector<QueueHandle*> FindColdFilter(uint64_t memory, const SequenceNumber& sn)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_){
    SingleQueue* queue = nullptr;
    std::vector<QueueHandle*> filters;
    for (int i = filters_number; i >= 1 && memory > 0; i--) {
      queue = queues_[i];
      queue->FindColdFilter(memory, sn, filters);
    }

    // if can not find cold filter reach memory byte
    // cancel this adjustment
    if(memory > 0) return {};
    return filters;
  }

  SingleQueue* FindQueue(QueueHandle* handle) EXCLUSIVE_LOCKS_REQUIRED(mutex_) {
    FilterBlockReader* reader = Value(reinterpret_cast<Handle*>(handle));
    if (reader != nullptr) {
      size_t number = reader->FilterUnitsNumber();
      SingleQueue* queue = queues_[number];
      return queue;
    }
    return nullptr;
  }

  bool CanBeAdjusted(const std::vector<QueueHandle*>& colds, QueueHandle* hot)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_){
    double original_ios = 0;
    for (QueueHandle* handle : colds) {
      if (handle && !handle->reader->CanBeEvict()) return false;
      original_ios += handle->reader->IOs();
    }
    original_ios += hot->reader->IOs();

    double adjusted_ios = 0;
    for (QueueHandle* handle : colds) {
      adjusted_ios += handle->reader->EvictIOs();
    }
    adjusted_ios += hot->reader->LoadIOs();

    if (adjusted_ios < original_ios) {
      adjustment_time_.fetch_add(1);
#ifdef USE_ADJUSTMENT_LOGGING
      LoggingAdjustmentInformation(colds.size(), hot->reader->FilterUnitsNumber(),
                                   hot->reader->AccessTime(),
                                   adjusted_ios, original_ios);
#endif
      return true;
    }
    return false;
  }

  void LoggingAdjustmentInformation(size_t cold_number, size_t hot_units_number,
                                    size_t access_time,
                                  double adjusted_ios, double original_ios)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_){
    Log(logger_,
        "Adjustment: Cold BF Number: %zu, Filter Units number of Hot BF: %zu, "
        "hot access frequency %zu, "
        "original ios: %f, "
        "adjusted ios: %f, "
        "adjust %zu times now",
        cold_number, hot_units_number, access_time,original_ios, adjusted_ios,
        adjustment_time_.load(std::memory_order_acquire));
  }

  Status LoadHandle(QueueHandle* handle) EXCLUSIVE_LOCKS_REQUIRED(mutex_){
    size_t number = handle->reader->FilterUnitsNumber();
    if(number < filters_number) {
      queues_[number]->Remove(handle);
      queues_[number + 1]->Append(handle);
      return handle->reader->LoadFilter();
    }
    return Status::Corruption("There is a full reader!");
  }

  Status EvictHandle(QueueHandle* handle) EXCLUSIVE_LOCKS_REQUIRED(mutex_){
    size_t number = handle->reader->FilterUnitsNumber();
    if(number >= 1) {
      queues_[number]->Remove(handle);
      queues_[number - 1]->Append(handle);
      return handle->reader->EvictFilter();
    }
    return Status::Corruption("There is not filter units to evict!");
  }

  void ApplyAdjustment(const std::vector<QueueHandle*>& colds,
                      QueueHandle* hot) EXCLUSIVE_LOCKS_REQUIRED(mutex_){
    Status s;
    for (QueueHandle* cold : colds) {
      s = EvictHandle(cold);
      if(!s.ok()){
#ifdef USE_ADJUSTMENT_LOGGING
        Log(logger_, "Adjustment: Evict colds handles failed, message is %s",
            s.ToString().c_str());
#endif
        adjustment_time_.fetch_sub(1);
        return ;
      }
    }

    s = LoadHandle(hot);
    if(!s.ok()){
#ifdef USE_ADJUSTMENT_LOGGING
      Log(logger_, "Adjustment: Load hot handle failed, message is %s",
          s.ToString().c_str());
#endif
      adjustment_time_.fetch_sub(1);
    }
  }

  void Adjustment(QueueHandle* hot_handle, const SequenceNumber& sn)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_){
    if (hot_handle && hot_handle->reader->CanBeLoaded()) {
      size_t memory = hot_handle->reader->OneUnitSize();
      std::vector<QueueHandle*> cold = FindColdFilter(memory, sn);
      if(cold.empty()) return ;
      if (CanBeAdjusted(cold, hot_handle)) {
        ApplyAdjustment(cold, hot_handle);
      }
    }
  }
};

MultiQueue* NewMultiQueue() { return new InternalMultiQueue(); }
}  // namespace leveldb