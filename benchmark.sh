#!/bin/bash

num=1700000000 #kv number in database
value_size=100
reads=10000000
bloom_bits=4 #bits per key for every filter unit

error="\e[31merror\e[0m:"
# false: use default db path
# true:  use user's db path
classify_path=false
# get db path from user
if [[ $# -eq 0 ]]; then
  echo -e "$error please entry model"
  exit 1
fi

if [[ $# -ge 1 ]]; then
  if [[ $1 == --model=* ]]; then
    model=$(echo $1 | cut -d= -f2)
    if [[ $model != "all" ]] && [[ $model != "read" ]] && [[ $model != "write" ]] && [[ $model != "clean" ]]; then
        echo -e "$error wrong model, must be read or write or clean, or all"
        exit 1
    fi
  else
    echo -e "$error Wrong model format, please append your model after --model= like --model=read!"
    exit 1
  fi
fi

if [[ $# -ge 2 ]]; then
  if [[ $2 == --db_path=* ]]; then
      db_path=$(echo $2 | cut -d= -f2)
      if [[ -d $db_path ]]; then
        echo "Your db path is in $db_path"
        classify_path=true
      else
        echo "There is no this path $db_path in your os!"
        exit 1
      fi
  else
    echo -e "$error Wrong db path format, please append your db path after --db_path= like --db_path=/your/db/path!"
    exit 1
  fi
fi

# clean and compile project
rm -rf build
mkdir build && cd build
# do not logging adjustment information
# the overhead is high
cmake -DCMAKE_BUILD_TYPE=Release .. && cmake --build .

benchmark="./db_bench --num=$num --print_process=0 --value_size=$value_size --reads=$reads --bloom_bits=$bloom_bits --cache_size=0"

# pass in db path
if [[ $classify_path == true ]]; then
  benchmark+=" --db=$db_path"
fi

# add model prefix
if [[ $model == "all" ]]; then
  benchmark+=" --benchmarks=fillrandom,readhot"
  $benchmark "--multi_queue_open=1"
  $benchmark "--multi_queue_open=0"
elif [[ $model == "read" ]]; then
  benchmark+=" --benchmarks=readhot"
  benchmark+=" --use_existing_db=1"
  $benchmark "--multi_queue_open=1"
  $benchmark "--multi_queue_open=0"
elif [[ $model == "write" ]]; then
  benchmark+=" --benchmarks=fillrandom"
  $benchmark
elif [[ $model == "clean" ]]; then
  benchmark+=" --benchmarks=crc32c"
  $benchmark
else
  echo -e "$error wrong model, must be read, write, clean or all!"
fi

# clean the cmake and make file
cd ../
rm -rf build