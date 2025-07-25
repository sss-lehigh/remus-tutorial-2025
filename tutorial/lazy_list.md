---
outline: deep
---

# Building A Concurrent Data Structure

There was some code in the previous step of the tutorial that you probably did
not like.  Remember this?

```c++
compute_threads[0]->Write<uint64_t>(remus::rdma_ptr<uint64_t>(ptr.raw() 
                                    + offsetof(SharedObject, values[i])), 
                                    (uint64_t)0);
```

Remus can hide all of this complexity for you, resulting in code that looks a
lot more  like traditional shared memory code.

## The `Atomic` template

The `Atomic` template is Remus's RDMA equivalent to `std::atomic`.  It forces
you to be explicit about loading and storing from memory, but then it hides all
offset computation, giving a more natural programming experience.  Start by
creating a new file called `lf_list.h`.

```c++
#pragma once

#include <memory>
#include <remus/remus.h>

using namespace remus;

/// A lock-based, sorted, list-based set with wait-free contains
///
/// @tparam K The type for keys stored in this map.  Must be default
///           constructable
template <typename K> class LazyListSet {

}
```

Inside of the list, define the type for nodes:

```c++
  using CT = std::shared_ptr<ComputeThread>;

  /// A node in the linked list
  struct Node {
    Atomic<K> key_;         // The key stored in this node
    Atomic<Node *> next_;   // Pointer to the next node
    Atomic<uint64_t> lock_; // A test-and-set lock

    /// Initialize the current node.  Assume that it is called with `this` being
    /// a remote pointer's value
    ///
    /// @param k  The key to store in this node
    /// @param ct The calling thread's Remus context
    void init(const K &k, CT &ct) {
      key_.store(k, ct);
      lock_.store(false, ct);
      next_.store(nullptr, ct);
    }

    /// Lock this node.  Assumes it is not already locked by the calling thread.
    /// Also assumes that it is called with `this` being a remote pointer's
    /// value
    ///
    /// @param ct The calling thread's Remus context
    void acquire(CT &ct) {
      while (true) {
        if (lock_.compare_exchange_weak(0, 1, ct) == 0) {
          break;
        }
        while (lock_.load(ct) == 1) {
        }
      }
    }

    /// Unlock this node.  Assumes it is called by the thread who locked it, and
    /// that the node is locked.  Also assumes that it is called with `this`
    /// being a remote pointer's value
    ///
    /// @param ct The calling thread's Remus context
    void release(CT &ct) { lock_.store(0, ct); }
  };
```

In a shared memory program, you'd probably want `key` to be `const`, but since
it's in remote memory, here you'll use the `Atomic` template for it, and for
`next`.  Similarly, in a shared memory program, you'd give `Node` a constructor,
instead of an `init()` function.  While it's possible to get the right behavior
with a constructor and *placement new*, Remus favors the use of separate `init`
functions.  Later in this step of the tutorial, you'll gain a better
understanding of why (briefly: because within `init()`, `this` is really an
`rdma_ptr` created via `compute_thread::New()`.)

As you probably expect, the head and tail sentinel pointers for this list will
also be `Atomic`:

```c++
  Atomic<Node *> head_; // The list head sentinel (use through This)
  Atomic<Node *> tail_; // The list tail sentinel (use through This)
```

Finally, you'll need to establish a special version of the `this` pointer:

```c++
  LazyListSet *This;    // The "this" pointer to use for data accesses
```

## Why `This` instead of `this`?

In the code you're about to write, you'll need to make sure to always use `This`
when accessing fields of the list, and `this` (which can often be omitted) when
referring to methods of the list.  This may seem confusing, but it is essential.

What's going to happen under the hood is that each Compute Node (or, if you
want, each Compute Thread) will have its own `LockFreeList` instance.  Using the
lowercase `this` pointer of that instance will provide you with a way of
referring to the addresses where the methods of that list can be found on the
local machine.  However, `this` is a local address, so it is not appropriate for
accessing the fields of the distributed object.  There are two mantras to keep
in mind:

1. **Data is distributed, code is replicated**.
2. **`This` is for data, `this` is for code**.

## Creating The Data Structure

As you've probably guessed from the difference between `this` and `This`, in
order to get a friendly programming experience, you're going to need to
construct your data structure in a special way.  Remember that in C++, `new`
actually does two things: it allocates memory and it initializes that memory.
In Remus, we essentially need two constructors:

- `New` (capitalized) will allocate *distributed* memory and initialize it
- `new` (not capitalized), through the regular constructor, will allocate
  *local* memory and initialize it.

Here's how to do it in the `LazyListSet`:

```c++
  /// Allocate a LazyList in remote memory and initialize it
  ///
  /// @param ct The calling thread's Remus context
  ///
  /// @return an rdma_ptr to the newly allocated, initialized, empty list
  static rdma_ptr<LazyListSet> New(CT &ct) {
    auto tail = ct->New<Node>();
    tail->init(K(), ct);
    auto head = ct->New<Node>();
    head->init(K(), ct);
    head->next_.store(tail, ct);
    auto list = ct->New<LazyListSet>();
    list->head_.store(head, ct);
    list->tail_.store(tail, ct);
    return rdma_ptr<LazyListSet>((uintptr_t)list);
  }

  /// Construct a LazyListSet, setting its `This` pointer to a remote memory
  /// location. Note that every thread in the program could have a unique
  /// LazyListSet, but if they all use the same `This`, they'll all access the
  /// same remote memory
  ///
  /// @param This A LockFreeList produced by a preceding call to `New()`
  LazyListSet(const remus::rdma_ptr<LazyListSet> &This)
      : This((LazyListSet *)((uintptr_t)This)) {}
```

In a few minutes, you'll write the code that calls these methods to construct a
list.  When you do, it will roughly follow these steps:

1. One thread will call `New` to allocate an object in shared memory.  Within
   `New`, `list` will be a remote address (e.g., its upper 16 bits specify a
   machine and segment, instead of being all zeroes).  `list->head_` is the
   remote address of an `Atomic` field.  `store` is a method of `Atomic` that
   uses that remote address to perform a write to the distributed memory.
   Finally, `list` is returned as an `rdma_ptr`.
2. Then every thread will use the `LazyListSet` constructor to make a local
   instance of the list where `This` is initialized.  All methods of the list
   will use `This` to interact with remote memory.

## Implementing Data Structure Methods

### Implementing `get()`

Now that `This` is initialized, the rest of the code is simple.  First, you
should implement the `get()` method.  The hardest part is dealing with marked
pointers (a key feature of the Lazy List).  Here are all the functions for
working with marked pointers:

```c++
  /// Report if `ptr` has its marked bit set
  bool is_marked(uintptr_t ptr) { return ptr & 1; }

  /// Clear the low bit of a uintptr_t
  uintptr_t unset_mark(uintptr_t ptr) { return ptr & (UINTPTR_MAX - 1); }

  /// Set the low bit of a uintptr_t
  uintptr_t set_mark(uintptr_t ptr) { return ptr | 1; }

  /// Clear the low bit of a pointer
  Node *make_unmarked(Node *ptr) { return (Node *)unset_mark((uintptr_t)ptr); }

  /// Set the low bit of a pointer
  Node *make_marked(Node *ptr) { return (Node *)set_mark((uintptr_t)ptr); }
```

With that out of the way, the code for `get()` is pretty much exactly what you'd
expect.  The only tricky part is remembering that your code needs to be careful
about using `This`.

```c++
  /// Report if a key is present in the set
  ///
  /// @param key The key to search for
  /// @param ct  The calling thread's Remus context
  /// @return True if it is found, false otherwise
  bool get(const K &key, CT &ct) {
    Node *HEAD = This->head_.load(ct);
    Node *TAIL = This->tail_.load(ct);
    Node *curr = HEAD->next_.load(ct);

    while (curr->key_.load(ct) < key && curr != TAIL)
      curr = make_unmarked(curr->next_.load(ct));
    return ((curr != HEAD) && (curr->key_.load(ct) == key) &&
            !is_marked((uintptr_t)curr->next_.load(ct)));
  }
```

### Inserting Data

In order to insert data, the Lazy List requires one more helper method, which
ensures that two nodes are consecutive, and that neither is deleted:

```c++
  /// Ensure that `pred` and `curr` are unmarked, and that `pred->next_`
  /// references `curr`
  ///
  /// NB: This assumes `pred` and `curr` are made from rdma_ptrs.
  ///
  /// @param pred The first pointer of the pair
  /// @param curr The second pointer of the pair
  /// @param ct   The calling thread's Remus context
  ///
  /// @return True if the validation succeeds, false otherwise
  bool validate_ptrs(Node *pred, Node *curr, CT &ct) {
    auto pn = pred->next_.load(ct);
    auto cn = curr->next_.load(ct);
    return (!is_marked((uintptr_t)pn) && !is_marked((uintptr_t)cn) &&
            (pn == curr));
  }
```

If you've implemented the lazy list for shared memory in C++, the only
difference in this code is that `.load` takes a thread context, instead of an
ordering constraint.  In general, converting data structures from shared memory
to Remus should be mechanical.

Now you can implement `insert()`:

```c++
  /// Insert a key into the set if it doesn't already exist
  ///
  /// @param key The key to insert
  /// @param ct  The calling thread's Remus context
  /// @return True if the key was inserted, false if it was already present
  bool insert(const K &key, CT &ct) {
    Node *HEAD = This->head_.load(ct);
    Node *TAIL = This->tail_.load(ct);
    Node *curr, *pred;
    int result, validated, not_val;
    while (true) {
      pred = HEAD;
      curr = make_unmarked(pred->next_.load(ct));
      while (curr->key_.load(ct) < key && curr != TAIL) {
        pred = curr;
        curr = make_unmarked(curr->next_.load(ct));
      }
      pred->acquire(ct);
      curr->acquire(ct);
      validated = validate_ptrs(pred, curr, ct);
      not_val = (curr->key_.load(ct) != key || curr == TAIL);
      result = (validated && not_val);
      if (result) {
        Node *new_node = ct->New<Node>();
        new_node->init(key, ct);
        new_node->next_.store(curr, ct);
        pred->next_.store(new_node, ct);
      }
      curr->release(ct);
      pred->release(ct);
      if (validated)
        return result;
    }
  }
```

### Removing a Key

You shouldn't need any more helper methods, but there is a small challenge that
awaits.  When inserting, you did not allocate a node until you were certain it
would be inserted, so there was no need to worry about deletion.  When removing
a key, deletion matters.  The challenge is that another thread (possibly on
another machine) could be reading a `Node` while it is being removed.

Unlike in shared memory, this can not lead to a page fault, because Remus does
not unmap distributed memory pages in the middle of execution.  But it still can
lead to ABA problems.  Sophisticated solutions use deferred reclamation
techniques, such as Epoch-based reclamation.  For RDMA programs, this is an open
research area.  Remus currently has a simple solution: you can `SchedReclaim` an
object, which places it into a list.  Then you can call `ReclaimDeferred` at
some point in the future (such as after a thread barrier) when you know it is
safe to reclaim *everything* in the list.

```c++
  /// Remove an entry from the set
  ///
  /// @param key The key to remove
  /// @param ct  The calling thread's Remus context
  /// @return True on success, false if the key was not present
  bool remove(const K &key, CT &ct) {
    Node *TAIL = This->tail_.load(ct);
    Node *pred, *curr;
    int result, validated, isVal;
    while (true) {
      pred = This->head_.load(ct);
      curr = make_unmarked(pred->next_.load(ct));
      while (curr->key_.load(ct) < key && curr != TAIL) {
        pred = curr;
        curr = make_unmarked(curr->next_.load(ct));
      }
      pred->acquire(ct);
      curr->acquire(ct);
      validated = validate_ptrs(pred, curr, ct);
      isVal = key == curr->key_.load(ct) && curr != TAIL;
      result = validated && isVal;
      if (result) {
        curr->next_.store(make_marked(curr->next_.load(ct)), ct);
        pred->next_.store(make_unmarked(curr->next_.load(ct)), ct);
        ct->SchedReclaim(curr);
      }
      curr->release(ct);
      pred->release(ct);
      if (validated)
        return result;
    }
  }
```

## Reclaiming A Data Structure

When the data structure is no longer in use, you'll want to reclaim it.  The
work of reclaiming distributed memory cannot be done from the destructor,
because the destructor might run in several threads, on several machines.  Such
a destructor should only reclaim the local object (the one that has a `This`
pointer), not the nodes in remote memory.  

The reclamation from remote memory can be done in sophisticated ways, so that
the reclaimed nodes are scattered across the different threads' free lists.  For
this example, we'll just do the easy thing and reclaim everything from a single
thread.  Note that if this code runs when there is no concurrency, it can
`Delete` nodes instead of using `SchedReclaim`.

```c++
  /// "Destruct" the list by reclaiming all of its nodes, and then reclaiming
  /// its `This` pointer.  It is assumed that this runs in a single-threaded
  /// context, so that memory can be immediately reclaimed.
  ///
  /// @param ct The calling thread's Remus context
  void destroy(CT &ct) {
    Node *curr = This->head_.load(ct);
    while (curr) {
      Node *next = curr->next_.load(ct);
      ct->Delete(curr);
      curr = next;
    }
    ct->Delete(This);
  }
```

## A Micro-Benchmark Harness

To test the code, it will be good to have a simple microbenchmark driver that
can run a batch of operations on the list.  Create a file called
`intset_test.h`, and paste in this code:

```c++
#pragma once

#include <atomic>
#include <csignal>
#include <random>

#include <remus/remus.h>

using std::uniform_int_distribution;
using namespace remus;
```

In order to run the benchmark in a variety of ways, you can create an ARGS
object and add it to Remus's command-line argument tool.  Define it like this:

```c++
// Command-line options for integer set microbenchmarks
constexpr const char *NUM_OPS = "--num-ops";
constexpr const char *PREFILL = "--prefill";
constexpr const char *INSERT = "--insert";
constexpr const char *REMOVE = "--remove";
constexpr const char *KEY_LB = "--key-lb";
constexpr const char *KEY_UB = "--key-ub";

/// An ARGS object for integer set microbenchmarks
auto DS_EXP_ARGS = {
    U64_ARG_OPT(NUM_OPS, "Number of operations to run per thread", 65536),
    U64_ARG_OPT(PREFILL, "Percent of elements to prefill the data structure",
                50),
    U64_ARG_OPT(INSERT, "Percent of operations that should be inserts", 50),
    U64_ARG_OPT(REMOVE, "Percent of operations that should be removes", 50),
    U64_ARG_OPT(KEY_LB, "Lower bound of the key range", 0),
    U64_ARG_OPT(KEY_UB, "Upper bound of the key range", 4096),
};
```

It will also be good to have a way of reporting run-time statistics.  This is
harder than it seems, because threads on different machines will need to combine
their statistics.  Start by creating a `Metrics` object:

```c++
/// Metrics is used to track events during the execution of an experiment
struct Metrics {
  size_t get_t = 0;       // calls to `get()` that returned true
  size_t get_f = 0;       // calls to `get()` that returned false
  size_t ins_t = 0;       // calls to `insert()` that returned true
  size_t ins_f = 0;       // calls to `insert()` that returned false
  size_t rmv_t = 0;       // calls to `remove()` that returned true
  size_t rmv_f = 0;       // calls to `remove()` that returned false
  size_t op_count = 0;    // expected total number of operations
  size_t write_ops = 0;   // number of RDMA writes
  size_t write_bytes = 0; // bytes written over RDMA
  size_t read_ops = 0;    // number of RDMA reads
  size_t read_bytes = 0;  // bytes read over RDMA
  size_t faa_ops = 0;     // number of RDMA FAA operations
  size_t cas_ops = 0;     // number of RDMA CAS operations

  /// Write this Metrics object to a file
  ///
  /// @param filename  The name of the file to write to
  /// @param duration  The duration of the experiment, in microseconds
  /// @param compute_thread A compute thread, for additional metrics
  void to_file(double duration, std::shared_ptr<ComputeThread> compute_thread) {
    std::ofstream file("metrics.txt", std::ios::out);
    file << "duration: " << duration << std::endl;
    file << "get_t: " << get_t << std::endl;
    file << "get_f: " << get_f << std::endl;
    file << "ins_t: " << ins_t << std::endl;
    file << "ins_f: " << ins_f << std::endl;
    file << "rmv_t: " << rmv_t << std::endl;
    file << "rmv_f: " << rmv_f << std::endl;
    file << "op_count: " << op_count << std::endl;
    file << "write: " << compute_thread->metrics_.write.ops << std::endl;
    file << "bytes_write: " << compute_thread->metrics_.write.bytes
         << std::endl;
    file << "read: " << compute_thread->metrics_.read.ops << std::endl;
    file << "bytes_read: " << compute_thread->metrics_.read.bytes << std::endl;
    file << "faa: " << compute_thread->metrics_.faa << std::endl;
    file << "cas: " << compute_thread->metrics_.cas << std::endl;
  }
};
```

We'll use this object as part of `IntSetTest`.  `IntSetTest` will do three
things: pre-fill the data structure, run a bunch of operations on it, and
aggregate results:

```c++
/// A per-thread object for running an integer set data structure experiment
///
/// @tparam S  The type of the data structure to test
/// @tparam K  The type of keys in that data structure
template <typename S, typename K> struct IntSetTest {
  using CT = std::shared_ptr<ComputeThread>;
  using AM = std::shared_ptr<ArgMap>;

  Metrics metrics_; // This thread's metrics
  S &set_;          // A reference to the set

  uint64_t thread_id_; // This thread's thread Id
  uint64_t node_id_;   // The Id of the compute node where the thread is running

  /// Construct an IntSetTest object
  ///
  /// @param set            A reference to the data structure
  /// @param thread_id      The thread's Id
  /// @param node_id        The Compute Node id
  IntSetTest(S &set, uint64_t thread_id, uint64_t node_id)
      : set_(set), thread_id_(thread_id), node_id_(node_id) {}

  /// Perform a distributed prefill of the data structure
  ///
  /// This uses the number of threads and the key range to compute a contiguous
  /// subrange of keys to insert, and has each thread insert their range
  ///
  /// @param ct     The calling thread's Remus context
  /// @param params The arguments to the program
  void prefill(CT &ct, AM &params) {
    // How big is the per-thread range of keys?
    auto total_threads =
        params->uget(CN_THREADS) *
        (params->uget(LAST_CN_ID) - params->uget(FIRST_CN_ID) + 1);
    auto key_lb = params->uget(KEY_LB);
    auto key_ub = params->uget(KEY_UB);
    auto range_length = (key_ub - key_lb + 1) / total_threads;
    // How many keys to insert within that range?
    auto num_keys =
        (key_ub - key_lb + 1) * params->uget(PREFILL) / 100 / total_threads;
    // Ok, do the insert
    auto start_key = key_lb + thread_id_ * range_length;
    auto end_key = start_key + range_length;
    auto step = (end_key - start_key) / num_keys;
    for (auto key = start_key; key < end_key; key += step) {
      K key_tmp = static_cast<K>(key);
      set_.insert(key_tmp, ct);
    }
  }

  /// Aggregate this thread's metrics into a global (remote memory) metrics
  /// object
  ///
  /// @param ct        The calling thread's Remus context
  /// @param g_metrics The global metrics object
  void collect(CT &ct, rdma_ptr<Metrics> g_metrics) {
    auto base = g_metrics.raw();
    ct->FetchAndAdd(rdma_ptr<uint64_t>(base + offsetof(Metrics, get_t)),
                    metrics_.get_t);
    ct->FetchAndAdd(rdma_ptr<uint64_t>(base + offsetof(Metrics, get_f)),
                    metrics_.get_f);
    ct->FetchAndAdd(rdma_ptr<uint64_t>(base + offsetof(Metrics, ins_t)),
                    metrics_.ins_t);
    ct->FetchAndAdd(rdma_ptr<uint64_t>(base + offsetof(Metrics, ins_f)),
                    metrics_.ins_f);
    ct->FetchAndAdd(rdma_ptr<uint64_t>(base + offsetof(Metrics, rmv_t)),
                    metrics_.rmv_t);
    ct->FetchAndAdd(rdma_ptr<uint64_t>(base + offsetof(Metrics, rmv_f)),
                    metrics_.rmv_f);
    ct->FetchAndAdd(rdma_ptr<uint64_t>(base + offsetof(Metrics, op_count)),
                    metrics_.op_count);
    ct->FetchAndAdd(rdma_ptr<uint64_t>(base + offsetof(Metrics, write_ops)),
                    ct->metrics_.write.ops);
    ct->FetchAndAdd(rdma_ptr<uint64_t>(base + offsetof(Metrics, write_bytes)),
                    ct->metrics_.write.bytes);
    ct->FetchAndAdd(rdma_ptr<uint64_t>(base + offsetof(Metrics, read_ops)),
                    ct->metrics_.read.ops);
    ct->FetchAndAdd(rdma_ptr<uint64_t>(base + offsetof(Metrics, read_bytes)),
                    ct->metrics_.read.bytes);
    ct->FetchAndAdd(rdma_ptr<uint64_t>(base + offsetof(Metrics, faa_ops)),
                    ct->metrics_.faa);
    ct->FetchAndAdd(rdma_ptr<uint64_t>(base + offsetof(Metrics, cas_ops)),
                    ct->metrics_.cas);
  }

  /// Run the experiment in this thread by executing a tight loop of operations,
  /// selected using the ratios given through command-line arguments.
  ///
  /// @param ct     The calling thread's Remus context
  /// @param params The arguments to the program
  void run(CT &ct, AM &params) {
    // set up a PRNG for the thread, and a few distributions
    using std::uniform_int_distribution;
    uniform_int_distribution<size_t> key_dist(params->uget(KEY_LB),
                                              params->uget(KEY_UB));
    uniform_int_distribution<size_t> action_dist(0, 100);
    std::mt19937 gen(std::random_device{}());
    // Get the target operation ratios from ARGS
    auto insert_ratio = params->uget(INSERT);
    auto remove_ratio = params->uget(REMOVE);
    auto lookup_ratio = 100 - insert_ratio - remove_ratio;
    uint64_t num_ops = params->uget(NUM_OPS);
    // Do a fixed number of operations per thread
    for (uint64_t i = 0; i < num_ops; ++i) {
      size_t key = key_dist(gen);
      size_t action = action_dist(gen);
      if (action <= lookup_ratio) {
        K key_tmp = static_cast<K>(key);
        if (set_.get(key_tmp, ct)) {
          ++metrics_.get_t;
        } else {
          ++metrics_.get_f;
        }
      } else if (action < lookup_ratio + insert_ratio) {
        K key_tmp = static_cast<K>(key);
        if (set_.insert(key_tmp, ct)) {
          ++metrics_.ins_t;
        } else {
          ++metrics_.ins_f;
        }
      } else {
        K key_tmp = static_cast<K>(key);
        if (set_.remove(key_tmp, ct)) {
          ++metrics_.rmv_t;
        } else {
          ++metrics_.rmv_f;
        }
      }
      ++metrics_.op_count;
    }
  }
};
```

## Finishing Up: `main()`

At last, you can write `lazy_list.cc` and add it to your `CMakeFiles`.  Most of
the code should be familiar.  At the beginning of `main()`, notice how two
different sets of `ARGS` are imported before the arguments are parsed.  That
suffices to turn on both sets of command-line options.

```c++
#include <memory>
#include <unistd.h>
#include <vector>

#include <remus/remus.h>

#include "cloudlab.h"
#include "intset_test.h"
#include "lazy_list.h"

int main(int argc, char **argv) {
  using Key_t = uint64_t;
  using set_t = LazyListSet<Key_t>;
  using test_t = IntSetTest<set_t, Key_t>;

  remus::INIT();

  // Configure and parse the arguments
  auto args = std::make_shared<remus::ArgMap>();
  args->import(remus::ARGS);
  args->import(DS_EXP_ARGS);
  args->parse(argc, argv);

  // Extract the args we need in EVERY node
  uint64_t id = args->uget(remus::NODE_ID);
  uint64_t m0 = args->uget(remus::FIRST_MN_ID);
  uint64_t mn = args->uget(remus::LAST_MN_ID);
  uint64_t c0 = args->uget(remus::FIRST_CN_ID);
  uint64_t cn = args->uget(remus::LAST_CN_ID);

  // prepare network information about this machine and about memnodes
  remus::MachineInfo self(id, id_to_dns_name(id));
  std::vector<remus::MachineInfo> memnodes;
  for (uint64_t i = m0; i <= mn; ++i) {
    memnodes.emplace_back(i, id_to_dns_name(i));
  }

  // Information needed if this machine will operate as a memory node
  std::unique_ptr<remus::MemoryNode> memory_node;

  // Information needed if this machine will operate as a compute node
  std::shared_ptr<remus::ComputeNode> compute_node;

  // Memory Node configuration must come first!
  if (id >= m0 && id <= mn) {
    // Make the pools, await connections
    memory_node.reset(new remus::MemoryNode(self, args));
  }

  // Configure this to be a Compute Node?
  if (id >= c0 && id <= cn) {
    compute_node.reset(new remus::ComputeNode(self, args));
    // NB:  If this ComputeNode is also a MemoryNode, then we need to pass the
    //      rkeys to the local MemoryNode.  There's no harm in doing them first.
    if (memory_node.get() != nullptr) {
      auto rkeys = memory_node->get_local_rkeys();
      compute_node->connect_local(memnodes, rkeys);
    }
    compute_node->connect_remote(memnodes);
  }

  // If this is a memory node, pause until it has received all the connections
  // it's expecting, then spin until the control channel in each segment
  // becomes 1. Then, shutdown the memory node.
  if (memory_node) {
    memory_node->init_done();
  }

  // If this is a compute node, create threads and run the experiment
  if (id >= c0 && id <= cn) {
    // Create ComputeThread contexts
    std::vector<std::shared_ptr<remus::ComputeThread>> compute_threads;
    uint64_t total_threads = (cn - c0 + 1) * args->uget(remus::CN_THREADS);
    for (uint64_t i = 0; i < args->uget(remus::CN_THREADS); ++i) {
      compute_threads.push_back(
          std::make_shared<remus::ComputeThread>(id, compute_node, args));
    }

    // Compute Node 0 will construct the data structure and save it in root
    if (id == c0) {
      auto set_ptr = set_t::New(compute_threads[0]);
      compute_threads[0]->set_root(set_ptr);
    }

    // Make threads and start them
    std::vector<std::thread> worker_threads;
    for (uint64_t i = 0; i < args->uget(remus::CN_THREADS); i++) {
      worker_threads.push_back(std::thread(
          [&](uint64_t i) {
            auto &ct = compute_threads[i];
            // Wait for all threads to be created
            ct->arrive_control_barrier(total_threads);

            // Get the root, make a local reference to it
            auto set_ptr = ct->get_root<set_t>();
            set_t set_handle(set_ptr); // calls the constructor for LockFreeList

            // Make a workload manager for this thread
            test_t workload(set_handle, i, id);
            ct->arrive_control_barrier(total_threads);

            // Prefill the data structure
            workload.prefill(ct, args);
            ct->arrive_control_barrier(total_threads);

            // Get the starting time before any thread does any work
            std::chrono::high_resolution_clock::time_point start_time =
                std::chrono::high_resolution_clock::now();
            ct->arrive_control_barrier(total_threads);

            // Run the workload
            workload.run(ct, args);
            ct->arrive_control_barrier(total_threads);

            // Compute the end time
            auto end_time = std::chrono::high_resolution_clock::now();
            auto duration =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    end_time - start_time)
                    .count();

            // Reclaim memory from prior phase
            ct->ReclaimDeferred();

            // "lead thread" will reclaim the data structure and then put a
            // global metrics object into the root
            if (id == c0 && i == 0) {
              set_handle.destroy(ct);
              auto metrics = ct->allocate<Metrics>();
              ct->Write(metrics, Metrics());
              ct->set_root(metrics);
            }
            ct->arrive_control_barrier(total_threads);

            // All threads aggregate metrics
            auto metrics = remus::rdma_ptr<Metrics>(ct->get_root<Metrics>());
            workload.collect(ct, metrics);
            ct->arrive_control_barrier(total_threads);

            // First thread writes aggregated metrics to file
            if (id == c0 && i == 0) {
              compute_threads[0]->Read(metrics).to_file(duration,
                                                        compute_threads[0]);
            }
          },
          i));
    }
    for (auto &t : worker_threads) {
      t.join();
    }
  }
};
```

It's good to quickly review the steps taken by each thread:

1. Since each Compute Node can lag behind others, you don't want to assume that
   Compute Node 0 initializes the root before threads on other Compute Nodes
   access it, so the thread code starts with a barrier.
2. Then all threads read the root to get an `rdma_ptr` to the list.  They make a
   local `LazyListSet` object from the root, called `set_handle`.
3. Each thread makes an `IntSetTest` object, so it can run data structure
   operations and collect metrics.
4. Threads work together to prefill the set to the fill rate determined by the
   command line arguments.
5. The start time is read *before* any thread starts the timed portion of the
   program.
6. Then all threads run the timed portion.
7. Once all threads exit the timed portion, the end time is read.  Since no
   thread is accessing the data structure, threads can `ReclaimDeferred`
   anything that was removed from the list during the timed portion.
8. Thread 0 of Compute Node 0 then reclaims all remote memory for the data
   structure.  It also creates a global Metrics object, and points the root to
   it.
9. All threads aggregate their metrics into the global `Metrics` object.
10. Thread 0 of Compute Node 0 reads the global metrics and writes the to a
    file.

While that may seem complicated, remember that it's not much different than
what's required in a shared memory micro-bechmark.  And, better yet, the code is
almost the same as in a shared memory micro-benchmark!

At this point, you should run some experiments with your code.  Be sure to vary
parameters, such as the number of compute nodes, the number of memory nodes, the
allocation policy, the number of memory segments per memory node, the number of
threads per compute node, and so forth.

You will almost certainly be disappointed by the throughput of your code.
Fortunately, the metrics provide some insight.  Are you observing many more RDMA
operations (reads, writes) than you expected?  Did you notice how each field of
a node is read via a different RDMA call?  Did you notice that in some cases, a
field is read more than once in the same loop iteration?  These performance bugs
are there to help you think about research opportunities.  In short, notice that
many powerful hardware-based memory features, like coarse granularity accesses,
caching, and prefetching, aren't free in RDMA systems.  Software versions of
these features are waiting for you to invent them!
