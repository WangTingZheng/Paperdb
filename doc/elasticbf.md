# Main Design in ElasticBF

## Fine-grained Bloom Filter Allocation

### BloomFilter

> related file:
> - util/bloom.cc
> - include/leveldb/filter_policy.h 

In a bloom filter, different keys will produce different hash values without considering hash collisions, but in ElasticBF, what we want to achieve is the design of a multi-bloom filter, so even if the same key, In different bloom filters, different hash values will be generated, so double hashing is used to calculate different initial hash function values for each filter unit, and then several hash function values are generated for a filter unit through this value:

```
            ----key----
            ↓         ↓
bitmap1 [01010110101001101010]

         --------key--
         ↓           ↓
bitmap2 [10010110101010000101]

······························
    
           ------key---
           ↓          ↓
bitmapn [10110110111001111001]
```

Two hash functions we selected:

```
f1(x) = Hash(x)
f2(x) = (f1(x) >> 17) | (f(x) << 15)
```

In a filter unit indexed to ``index``, to calculate the hash value generated by the hash function, LevelDB also uses double hashing, that is, to generate two hash values, and then generate k hash values based on the two hash values.

```
f_original_hash(x, index) = [f1(x) + index * f2(x)]
```
Where index is the subscript of the filter unit, and based on this hash value, we can generate an auxiliary function:

```
f_delta(x, index) = (f_original_hash(x, index) >> 17) | (f_original_hash(x, index) << 15)
```
Based on these two hash values, we can calculate the hash function value of the filter unit indexed with index k:

```
f_hash(x, index) = f_original_hash(x, index) + k * f_delta(x, index)
```

**Note**: in fact, the false positive rate of the bloom filter is not only related to the configuration of the bitmap, but also to the hash function. The difference in the seed of the hash function will affect its collision rate. The false positive rate of LevelDB's default hash function seed is not low in the Doblong filter scenario, so we modified the seed of the hash function.

### FilterBlock

> related file:
> - table/filterblock.cc
> - table/filterblock.h

The format of the new filterblock is as follows, divided into two parts, the disk part and the memory part. When reading the filterblock, only the memory part will be read. The memory part will read several from the disk according to the offset and size of the saved filter unit. When querying, the filterblock will traverse all the filter units that have been loaded. Since the Bloom filter has no false negative rate, as long as a Bloom filter determines that the key does not exist, then this key will definitely not exist..

```
<beginning_of_file>
[filter unit 1] // same size but different contents
[filter unit 2]
...
[filter unit N]                              in disk
----------------------------------------------------
[filter offset1]                             (meta) in memory
[filter offset2] // common to all filter units
...
[filter offsetM]
[filter unit's len] // desgin for last bitmap reading
[offset in disk] // first filter unit offset in disk
[size]   // first filter unit size in disk
[loaded] // filter unit number we should load when read table
[number] // all filter units number 
[baselg] 
<end_of_file>
```

**Note**: It is worth noting that the size of each filter unit is the same, and the filter units are closely arranged, so we only need to save the offset and size of the first filter unit. In the process of reading, crc needs to be used for verification to prevent reading the wrong bitmap and misjudgment, so that the existing key is judged to not exist, resulting in data loss.

**Note**: In the process of implementation, because only TableBuilder saves the entire SSTable file offset, when we generate all the filter units, we need to return to TableBuilder for persistence, get the offset and size of the filter unit from TableBuilder, and then return to FilterBlock to complete the construction of the memory part.

**Note**: LevelDB uses InternalFilterPolicy to wrapper the Bloom Filter. This filter will convert the InternalKey with the serial number and KV type into the user key passed by the user, and then insert it into the Bloom filter to build a bitmap, but in ElasticBF, We need to generate a bitmap for a set of keys, which will cause the InternalKey that has been converted to the user key to be converted again, resulting in a parsing error. The solution is to add a flag to convert only when the first filter unit is generated. Just see this [code](https://github.com/WangTingZheng/Paperdb/blob/242b1b92cf97453d7750ea6f630cb490bb14feb7/db/dbformat.cc#L108)

### Meta Index Block

> related file:
> - table/table.cc
> - table/table_builder.cc

The implementation of ElasticBF does not require any modification to the Meta index block, but because we have re-modified the format of the FilterBlock, the filter unit that occupies most of the space is saved in the disk. We only need to read the meta data from the disk. Dynamically load the filter unit in the disk, which makes the filterblock very small. Obtain the offset and size of the filter block from the Meta Index Block, and then read the filter block to the disk according to them, it is a waste of IO, so I choose to save the filterblock directly to the value in Meta Index Block. There is no need to read FilterBlock from disk anymore, after reading meta index block.

```
LevelDB version:

key
↓
Meta Index Block -> offset, size
                    ↓  （one disk io）
                    FilterBlock Contents
                    ↓
                    FilterBlockReader
------------------------------------------      
ElasticBF version:

key
↓                   (save io here)
Meta Index Block -> FilterBlock Contents
                    ↓ 
                    FilterBlockReader
```

### Table

> related file:
> - table/table.cc
> - table/table_builder.cc

Table adapts to two situations, one is that we use the default way of LevelDB to read filterblock, the other is to use multi_queue to manage filterblock, and the two switch to option.multi_queue is set.

When the option of the multi_queue is nullptr, we will open the Table, directly from the disk to read data to create filterblockreader object, if not nullptr, indicating the need to filterblockreader by the multi_queue management, then we open the Table, from disk to create filterblockreader read data object is inserted into the multi_queue, while returning a handle containing the reader for query use.

When the table is released due to program exit or TableCache replacement policy, the reader saved in the table or the handle saved multi_queue will be released.

## Hotness Identification

> related file:
> - table/filterblock.cc
> - table/filterblock.h

When the key is passed in the KeyMayMatch in the filterblockreader, we will parse the Sequence Number from the InternalKey, update the sequence in the filterblockreader, and determine whether the FilterBlock is cold, we pass in the Sequence Number of the read request at that time. If it is greater than or equal to the sequence of the filterblockreader plus life_time, it means that this filterblockreader has not been accessed within the life time, which is a cold FilterBlock.

## Bloom Filter Management in Memory

> related file:
> - util/multi_queue.cc
> - util/multi_queue.cc

### Main components
Multi Queue structure just like this:
```
HashTable:

   -----------------------
   | key  | QueueHandle* |----
   -----------------------   |
   | key  | QueueHandle* |   |
   ----------------------    |
   | key  | QueueHandle* |   |
   ----------------------    |
                             |
                             ↓
                         QueueHandle-------
                           ↑ ↑            ↓
                           | |          reader  
                           | | equal   
                    ----->     ----->      ---->
SingleQueue1     mlu      node1       node2     lru
                    <----      <-----      <----

                    ----->     ----->      ---->
SingleQueue2     mlu      node1       node2     lru
                    <----      <-----      <----
                    
                    ----->     ----->      ---->
SingleQueue3     mlu      node1       node2     lru
                    <----      <-----      <----

```

- QueueHandle: A node in a linked list, encapsulating filterblockreader
- SingleQueue: An LRU linked list containing filterblockreaders loaded with the same number of filter units. The accessed QueueHandle will be updated to the previous node of the header node
- MultiQueue: a multi-level queue composed of multiple linked lists, from the linked list with more filter units to the linked list with fewer filter units, start querying the cold FilterBlockReader from the LRU section of the linked list, and query the required cold FilterBlockReader with minimal cost

### Insert and search in SingleQueue
Empty SingeQueue:
```
   (next)
mlu----->lru 
↑         ↓(prev)
----------|
```

Insert or move a new Handle to MLU end:

```
new--------
↓(prev)   ↓(next)
mlu----->lru 
↑         ↓
----------|

---------------------------------

       ----------new--------
 (next)↑ ↓(prev)   (next)↓ ↑(prev)
 mlu                     lru 
```

Find Cold handle from lru to mru end

```
   ----->     ----->      ---->
mlu      node1       node2     lru
   <----      <-----      <----
         [<--------------]
              search
```

**Node**: Internal node mlu/lru has no key, call Key() will be crashed.

### Adjustment policy

- Collect Cold FilterBlockReader: Calculate how much memory is required to load the filter unit of a hot filterblockreader, from the list with more filter units to the list with less, the LRU end of the linked list to the MRU end, and judge whether a FilterBlockReader is cold through SequenceNumber. If it is cold, save it. If you can collect no less than the memory of the cold Reader loading the filter unit of a hot reader, return the reader's collection. If it is not complete, return empty.
- Determine whether it should be adjusted: If no suitable cold reader is found, it is directly considered unadjustable. If there is, it is judged that all of these cold Readers evict a filter unit, and the resulting disk IO can be offset by loading a hot Reader.
- Apply adjustment: If should be adjusted, evict all the cold readers with a filter unit and load the hot reader with a filter unit.

**Note**: The total memory footprint of the filter unit of the evicted cold reader cannot be less than the memory footprint of a filter unit of the loaded hot reader, and a little more is fine.

**Note**: A cold reader with only one filter unit cannot be selected, and the false positive rate without a filter unit is not easy to calculate.

**Note**: We add a lock into multi queue to support multi threads, LevelDB's benchmark is under multi threads.