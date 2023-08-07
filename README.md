# ElasticBF
[![ci](https://github.com/google/leveldb/actions/workflows/build.yml/badge.svg)](https://github.com/google/leveldb/actions/workflows/build.yml)

**[ElasticBF][1]** moves cold SSTable's bloom filter to hot SSTable's bloom filter to reduce extra disk io overhead without increasing memory overhead. Short introduction by paper's author can be saw in this [video](https://www.youtube.com/watch?v=8UBx3GCep3A). In addition to that, we also ensure code quality by utilizing **unit test**, **google sanitizers**, and **GitHub Actions** with support for multiple operating systems and compilers. Comparing changes with LevelDB just see [``this place``](https://github.com/google/leveldb/compare/main...WangTingZheng:Paperdb:elasticbf-dev?expand=1). A fork version of YCSB to support ElasticBF is [here](https://github.com/WangTingZheng/YCSB-cpp).

# Features
![The architecture of ElasticBF](https://github.com/WangTingZheng/Paperdb/assets/32613835/cb3278c6-9782-48b1-bda4-2051713a6a97)

* [[Github pr](https://github.com/WangTingZheng/Paperdb/pull/60)] Based feature:
  * Fine-grained Bloom Filter Allocation
  * Hotness Identification
  * Bloom Filter Management in Memory 
  * Background thread to load filter
* DirectIO implementation in LevelDB
  * [[Github pr](https://github.com/WangTingZheng/Paperdb/pull/61)]: Direct IO prototype
  * [[Github pr](https://github.com/WangTingZheng/Paperdb/pull/67)]: Direct IO speed tester
* [[Github pr](https://github.com/WangTingZheng/Paperdb/pull/63)] Hotness inheritance

> More details see in [doc/elasticbf.md](./doc/elasticbf.md), Chinese version see in [feishu](https://o444bvn7jh.feishu.cn/docx/XlBldwKc2oOTGMxPpLlckKLunvd), If you are interested in gaining a deeper understanding of the paper, please do not hesitate to contact me. I have added **``extensive annotations``** to the original paper, and I have recorded **``8 hours``** of lecture videos to elaborate on my interpretation of the paper.


# Main changed files

* **util/bloom.cc**: Generate and read multi filter units' bitmap for a scope of keys **(97% lines unit test coverage)**
* **table/filterblock.cc**: Manage multi filter units in disk, update the hotness of the SSTable. **(94% lines unit test coverage)**
* **table/table.cc**: Check if the key is existed using filterblock **(97% lines unit test coverage)**
* **table/table_builder.cc**: Construct filter block **(89% lines unit test coverage)**
* **util/multi_queue.cc**: Manage filter units in memory to reduce adjust overhead **(98% lines unit test coverage)**
* **util/condvar_signal.h** : A wrapper for signal all threads waiting for using filterblock reader by condvar using RAII, similar to unique_ptr
* **util/read_buffer.h**: A RAII style class for manage allocated string in read function in RandomAccessFile
* **uti/env_posix_direct_io_test.cc**: A micro benchmark for getting latency of buffer IO and direct IO

# Performance

Adjustment apply 1040 times during Lookup, saved 8,279,190 IOs, improve throughput from 787.27op/sec to 968.716 op/sec, speed up 23% read performance:  

<img src="/doc/performance.png" alt="performance" style="width: 35%; height: 35%;">

---
# ToDo in ElasticBF
- ~~Using multi thread to speed up filter units loading in multi queue, see [about implementing multi threads](https://github.com/WangTingZheng/Paperdb/discussions/14).~~
- Using shared hash in this [paper][3] to reduce multi bloom filter look up overhead.
- ~~Hotness inheritance after compaction in [ATC version of paper][4], see [about implementing hotness inheritance](https://github.com/WangTingZheng/Paperdb/discussions/13) in discussions.~~
- ~~Using [perf][8] tool to find code can be optimized.~~
- Reimplement ElasticBF to get rid of the unit test, sanitizers and benchmark, see [about reimplementing](https://github.com/WangTingZheng/Paperdb/discussions/15)
- ~~Support YCSB, should pay attention to [FalsePositiveRate function](https://github.com/WangTingZheng/Paperdb/blob/242b1b92cf97453d7750ea6f630cb490bb14feb7/db/c.cc#L140) in db/c.cc~~
- ~~Fork a [YCSB-CPP](https://github.com/ls4154/YCSB-cpp) to fit ElasticBF~~, Just see my [YCSB](https://github.com/WangTingZheng/YCSB-cpp)
- More ToDo, please see [Github project](https://github.com/users/WangTingZheng/projects/17/views/1)

# Next Paper

Next paper to implement maybe [AC-Key][5] or [HotRing][6] or [WiscKey][7], see [some ideas about implementing Wisckey](https://github.com/WangTingZheng/Paperdb/discussions/12). For Hotring, there is a [hashtable](https://github.com/facebook/rocksdb/tree/main/utilities/persistent_cache) in RocksDB with unit test and benchmark, we can modified it to implement Hotring. One more thing, The Skiplist maybe replaced by [ART(adaptive radix tree)][9].


---

# Building

Get the source from github, elasticbf's code is in elasticbf-dev branch:
```shell
git clone --recurse-submodules https://github.com/WangTingZheng/Paperdb.git
cd Paperdb
git checkout elasticbf-dev
```

This project supports [CMake](https://cmake.org/) out of the box. Quick build:

```shell
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release .. && cmake --build .
```

# Bloom filter adjustment logging

Turn on logging by passing parameter in cmake command:

```shell
cmake -DCMAKE_BUILD_TYPE=Debug -DUSE_ADJUSTMENT_LOGGING=ON .. && cmake --build .
```
or in release mod:

```shell
cmake -DCMAKE_BUILD_TYPE=Release -DUSE_ADJUSTMENT_LOGGING=ON .. && cmake --build .
```

You can check out adjustment information in ``/LOG`` file in your db dictionary, in ``/tmp`` in default setting:
```shell
cd /your/db/dictionary
cat LOG | grep "Adjustment:"
```
**Note1**: You must delete cmake cache file(such as cmake-build-debug) before switching ```USE_ADJUSTMENT_LOGGING``` parameters' value

**Note2**: When db is destroyed, We will log sum of adjustment time.

# Unit test and benchmark

## unit test:
```shell
cd build
./leveldb_tests
./env_posix_test
./c_test
```
Only run one test:
```shell
./leveldb_tests --gtest_filter=DBTest.BloomFilter
```

## benchmark
```shell
cd build
./db_bench
```

## benchmark parameters:

close multi_queue, use default way to get filter 
```shell
./db_bench --multi_queue_open=0
```
close info printing in FinishedSingleOp
```shell
./db_bench --print_process=0
```
**Note**: We disable FinishedSingleOp printf in CI to track error easier.

## benchmark bash shell
we use a bash shell to wrapper benchmark command, when run benchmark through benchmark.sh, you can pass in your own dictionary, pass ``write`` to bash will run fillrandom benchmark:
```shell
./benchmark.sh --model=write --db_path=/your/db/path
```

If you want to use default dictionary, just run:
```shell
./benchmark.sh --model=write
```
pass ``read`` to bash will run readrandom benchmark:

```shell
./benchmark.sh --model=read
```
pass ``clean`` to bash will run destroy db, file will be deleted:
```shell
./benchamrk.sh --model=clean
```
pass ``all`` to bash wll run fillrandom and readrandom together
```shell
./benchmark.sh --model=all
```
## benchmark setup

### Hardware

* CPU:        32 * 13th Gen Intel(R) Core(TM) i9-13900K
* CPUCache:   36864 KB
* SSD:        Samsung SSD 870(4TB)

### parameters
* 100GB kv in database
* 10 million point lookup
* 4 bits per key in one filter unit
* 6 filter units for one SSTable
* Load 2 filters at the beginning
* 128Byte KV
* On release model
* Snappy compression is not enabled
* use zipfian, constant is 0.99

# Google sanitizers

Sanitizers only support of Debug mod, and you must turn it on by yourself:
```shell
cmake -DCMAKE_BUILD_TYPE=Debug -DUSE_SAN_ADD=ON .. && cmake --build .
```
use this to open thread sanitizers:
```shell
-DUSE_SAN_THR=ON
```

use this to open undefined behaviour sanitizers
```shell
-DUSE_SAN_UB=ON
```
Why google sanitizers? [Google sanitizers is faster more than 10x with vaigrind][2].

**Note**: Do not open Address and thread sanitizers together.

**Note:** We open it by default in order to check memory leak in CI.

# CI in Github Action

We use github action to test code in different os, compiler and build type(Debug and Release), those jobs will be run in CI, all jobs will be checked by address sanitizers:
- Run Tests: Run all test, include leveldb_tests, env_posix_test and c_test
- Run LevelDB Benchmarks: benchmark in ```benchmarks/db_bench.cc```
- Run SQLite Benchmarks: benchmark in ```benchamrks/db_bench_sqlite3.cc```
- Run Kyoto Cabinet Benchmarks in ```benchmarks/db_bench_tree_db.cc```

We also use thread sanitizers to check the code for the jobs mentioned above in linux debug model:

- Run Tests with thread sanitizer
- Run LevelDB Benchmarks with thread sanitizer
---
# Related PR to LevelDB

I created two pr to LevelDB during implementing ElasticBF:

- [[**Not merged yet**] Add a flag to close FinishedSignleOp printf in CI #1131](https://github.com/google/leveldb/pull/1131)
- [[**Not merged yet**] fix memory leak in kyoto cabinet benchmarks #1128](https://github.com/google/leveldb/pull/1128)
- [[**Not merged yet**] fix nullptr ub in hash function #1136](https://github.com/google/leveldb/pull/1136)

**Note:** Unfortunately, LevelDB is receiving very limited maintenance, so, those pr may not be merged.

[1]: https://www.usenix.org/conference/hotstorage18/presentation/zhang "Zhang Y, Li Y, Guo F, et al. ElasticBF: Fine-grained and Elastic Bloom Filter Towards Efficient Read for LSM-tree-based KV Stores[C]//HotStorage. 2018."

[2]: https://www.bilibili.com/video/BV1YT411Q7BU "王留帅、徐明杰-Sanitizer 在字节跳动 C C++ 业务中的实践.BiliBili. 2023."

[3]: https://dl.acm.org/doi/10.1145/3465998.3466002 "Zhu Z, Mun J H, Raman A, et al. Reducing bloom filter cpu overhead in lsm-trees on modern storage devices[C]//Proceedings of the 17th International Workshop on Data Management on New Hardware (DaMoN 2021). 2021: 1-10."

[4]: https://www.usenix.org/conference/atc19/presentation/li-yongkun "Li Y, Tian C, Guo F, et al. ElasticBF: Elastic Bloom Filter with Hotness Awareness for Boosting Read Performance in Large Key-Value Stores[C]//USENIX Annual Technical Conference. 2019: 739-752."

[5]: https://www.usenix.org/conference/atc20/presentation/wu-fenggang "Wu F, Yang M H, Zhang B, et al. AC-key: Adaptive caching for LSM-based key-value stores[C]//Proceedings of the 2020 USENIX Conference on Usenix Annual Technical Conference. 2020: 603-615."

[6]: https://www.usenix.org/conference/fast20/presentation/chen-jiqiang "Chen J, Chen L, Wang S, et al. HotRing: A Hotspot-Aware In-Memory Key-Value Store[C]//FAST. 2020: 239-252."

[7]: https://dl.acm.org/doi/10.1145/3033273 "Lu L, Pillai T S, Gopalakrishnan H, et al. Wisckey: Separating keys from values in ssd-conscious storage[J]. ACM Transactions on Storage (TOS), 2017, 13(1): 1-28."

[8]: https://zhuanlan.zhihu.com/p/639996512 "Ash1n2. 差分火焰图，让你的代码优化验证事半功倍. Zhihu. 2023."

[9]: https://ieeexplore.ieee.org/abstract/document/6544812/. "Leis V, Kemper A, Neumann T. The adaptive radix tree: ARTful indexing for main-memory databases[C]//2013 IEEE 29th International Conference on Data Engineering (ICDE). IEEE, 2013: 38-49."
