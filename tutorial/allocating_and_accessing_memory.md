---
outline: deep
---

# Allocating And Accessing Memory

Now that you've successfully connected the Memory and Compute nodes, it's time
to instruct some threads to use the distributed memory.  In this step of the
tutorial, it will be important to think about timing issues.  Otherwise, you
might end up accessing memory before it has been initialized, or after it has
been destroyed.

## A Starting Point

In the previous step of the tutorial, you wrote a program that used
`remus::ARGS` to configure Memory Nodes and Compute Nodes.  This step of the
tutorial builds on that program:

```c++
#include <memory>
#include <remus/remus.h>
#include <vector>

#include "cloudlab.h"

int main(int argc, char **argv) {
  remus::INIT();

  // Configure and parse the arguments
  auto args = std::make_shared<remus::ArgMap>();
  args->import(remus::ARGS);
  args->parse(argc, argv);
  if (args->bget(remus::HELP)) {
    args->usage();
    return 0;
  }
  args->report_config();

  // Extract the args for determining names and roles
  uint64_t id = args->uget(remus::NODE_ID);
  uint64_t m0 = args->uget(remus::FIRST_MN_ID);
  uint64_t mn = args->uget(remus::LAST_MN_ID);
  uint64_t c0 = args->uget(remus::FIRST_CN_ID);
  uint64_t cn = args->uget(remus::LAST_CN_ID);
  uint64_t threads = args->uget(remus::CN_THREADS);

  // prepare network information about this machine and about memnodes
  std::vector<remus::MachineInfo> memnodes;
  for (uint64_t i = m0; i <= mn; ++i) {
    memnodes.emplace_back(i, id_to_dns_name(i));
  }

  // Compute the name of this machine
  remus::MachineInfo self(id, id_to_dns_name(id));

  // Configure as a MemoryNode?
  std::unique_ptr<remus::MemoryNode> memory_node;
  if (id >= m0 && id <= mn) {
    memory_node.reset(new remus::MemoryNode(self, args));
  }

  // Configure as a ComputeNode?
  std::shared_ptr<remus::ComputeNode> compute_node;
  if (id >= c0 && id <= cn) {
    compute_node.reset(new remus::ComputeNode(self, args));
    // NB:  If this ComputeNode is also a MemoryNode, then we need to pass the
    //      rkeys to the local MemoryNode.  There's no harm in doing them first.
    if (memory_node.get() != nullptr) {
      compute_node->connect_local(memnodes, memory_node->get_local_rkeys());
    }
    compute_node->connect_remote(memnodes);
  }

  // Reclaim threads when all connections have been made
  if (memory_node) {
    memory_node->init_done();
  }

  if (id >= c0 && id <= cn) {
    // Create a context for each Compute Thread
    std::vector<std::shared_ptr<remus::ComputeThread>> compute_threads;
    for (uint64_t i = 0; i < threads; ++i) {
      compute_threads.push_back(
          std::make_shared<remus::ComputeThread>(id, compute_node, args));
    }

    // TODO: workload goes here
  }
}
```

:::warning
It's likely that you'll write some buggy code during this step.  There is a
`make debug` target in the `Makefile`, which may be useful if you need to use
the `cl.exe run-debug` command to debug your code with `gdb`.
:::

## Constructing A Shared Object

Before any threads can interact via the distributed shared memory, that memory
needs to be configured.  The `MemoryNode` constructors perform generic
configuration, but you'll probably need to do some configuration specific to the
data structure and experiment.

In addition to the correctness requirement of performing the configuration
before threads run, there is also a performance challenge.  Once an object is
allocated on a specific machine, that machine remains its effective "home" for
the rest of its lifetime.  If you initialize a data structure *entirely on one
machine*, that machine can become an artificial contention point during program
execution!

Remus includes a few allocation policies, but in truth this is an area of active
research.  Your best bet is probably to construct the initial set of shared
objects in a few steps:

1. Construct the root object from thread 0 of Compute Node 0.
2. Publish that root object so all threads can see it.
3. Synchronize all threads with a barrier.
4. Use a set of threads, distributed across all Compute Nodes, to construct the
   rest of the shared objects.
5. Synchronize all threads with a barrier.

That process is more complex than you'll need for this step of the tutorial.  In
this step, you'll just make one shared object.

Before the `main` function, define your shared object as an array of $1024$
integers.

```c++
struct SharedObject {
  uint64_t value[1024];
};
```

Then create the object and initialize it as the first part of your workload:

```c++
    // The process is still sequential.  The main thread on Compute Node 0 will
    // create the shared object, using one of the available ComputeThread
    // objects:
    if (id == c0) {
      auto ptr = compute_threads[0]->allocate<SharedObject>();
      for (unsigned i = 0; i < 1024; ++i) {
        // Since the memory isn't local, we need to initialize it explicitly
        compute_threads[0]->Write<uint64_t>(
            remus::rdma_ptr<uint64_t>(ptr.raw() +
                                      offsetof(SharedObject, values[i])),
            (uint64_t)0);
      }
      // Make the SharedObject visible through the global root pointer, which is
      // at Memory Node 0, Segment 0.
      compute_threads[0]->set_root(ptr);
    }
```

When you call `allocate`, Remus will use the `--alloc-pol` flag to determine
which Memory Node the current Compute Thread should allocate from.

## `rdma_ptr<t>`, `Read`, `Write`, and `CompareAndSwap`

The Segments at a Memory Node make use of addresses that are local to that node,
and hence not globally unique.  This creates a challenge, since the union of all
Segments, on all Memory Nodes, comprises the distributed shared memory space.
To make things easier, Remus takes advantage of the fact that the high bits of
64-bit addresses on x86 processors are not used, and repurposes them as a Memory
Node / Segment identifier.  It then hides the logic behind a pointer template,
`rdma_ptr<T>`.

The easiest way to think about `rdma_ptr<T>` is that you should use it as a
replacement for pointers in your program.  If you want to allocate from
distributed memory, use `ComputeThread::allocate`, which returns an
`rdma_ptr<T>`. If you want to save a pointer as a field of an object, the field
type should be `rdma_ptr<T>`. Then, to read from a pointer or write to the
memory it references, use the corresponding `ComputeThread` methods: `Read<T>`,
`Write<T>`, `CompareAndSwap<T>`, and `FAA<T>`.

For structs and arrays, `rdma_ptr<T>` does not yet automatically compute
offsets.  Instead, the code for initializing them uses `offsetof` to compute the
address of each position.

## Launching Threads

Now that you've got a properly initialized object, and it's globally accessible
through the root pointer (which is just a dedicated 64-bit slot in the control
block of Memory Node 0), it's time to start some threads:

```c++
    // Barriers will need to know the total number of threads across all
    // machines
    uint64_t total_threads = (cn - c0 + 1) * args->uget(remus::CN_THREADS);

    // Create threads on this ComputeNode.  The main thread will stop doing any
    // work now.
    std::vector<std::thread> local_threads;
    for (uint64_t i = 0; i < threads; ++i) {
      uint64_t local_id = i;
      uint64_t global_thread_id = id * threads + i;
      auto t = compute_threads[i];
      local_threads.push_back(std::thread([t, global_thread_id, total_threads,
                                           local_id, id]() {
        // All threads, on all machine, synchronize here.  This ensures the
        // initialization is done and the root is updated before any thread
        // on any machine passes the next line.
        t->arrive_control_barrier(total_threads);
        auto root = t->get_root<SharedObject>();
        // ...
      }));
    }
```

This code uses the sense-reversing barrier in Memory Node zero to ensure that no
thread reads the `root` until all threads have reached the barrier.
Transitively, the threads on Compute Node zero won't even run until after
Compute Node zero has written the `root`, so there's a nice guarantee that no
thread will read the root before it's initialized.

And, of course, after the loop, you'll want to join on all of the threads:

```c++
    for (auto &t : local_threads) {
      t.join();
    }
```

## A Simple Workload

Now it's time to do some reading and writing of the `SharedObject`:

```c++
        // Read from a unique location in SharedObject, ensure it's 0
        auto my_loc = remus::rdma_ptr<uint64_t>(
            root.raw() + offsetof(SharedObject, values[global_thread_id + 1]));
        uint64_t my_val = t->Read<uint64_t>(my_loc);
        if (my_val != 0) {
          REMUS_FATAL("Thread {}: Read observed {}", global_thread_id, my_val);
        }

        // Write a unique value to that unique location
        t->Write<uint64_t>(my_loc, global_thread_id);

        // Use a CompareAndSwap to increment it by 1
        bool success =
            global_thread_id ==
            t->CompareAndSwap(my_loc, global_thread_id, global_thread_id + 1);
        if (!success) {
          REMUS_FATAL("Thread {}: CAS failed", global_thread_id);
        }

        // Wait for all threads to finish their work
        t->arrive_control_barrier(total_threads);
```

`my_loc` is a convenience pointer, which avoids having to re-compute the same
offset for each `Read`, `Write`, and `CompareAndSwap`.  Reading and writing have
the same general format as in other concurrent programming environments, such as
transactional memory.  As for `CompareAndSwap`, it's roughly equivalent to
`std::atomic<T>::compare_exchange_strong`, but it returns the value it observed,
not a boolean value.

## Validating The Result

It's always a good idea to verify the result of a program's computation.  In
this case, thread 0 of Compute Node 0 can read from the `SharedObject` to
ensure that each thread did what it was supposed to do:

```c++
        // Now thread 0 can check everything:
        if (global_thread_id == 0) {
          for (uint64_t i = 1; i < total_threads + 1; ++i) {
            auto found = t->Read<uint64_t>(remus::rdma_ptr<uint64_t>(
                root.raw() + offsetof(SharedObject, values[i])));
            if (found != i) {
              REMUS_FATAL("In position {}, expected {}, found {}", i, i, found);
            }
            REMUS_INFO("All checks succeeded!");
          }
        }
```

## Cleaning Up

Finally, before threads shut down, it's a good idea to clean up the data
structure:

```c++
        // Reclaim the object before terminating
        if (global_thread_id == 0) {
          t->deallocate(root);
          t->set_root(remus::rdma_ptr<SharedObject>(nullptr));
        }
```

Note that this code runs *before* the `ComputeThread` is reclaimed.  This is
especially important since `ComputeThread` destruction triggers `MemoryNode`
reclamation... you wouldn't want reclamation to be attempted after the
`MemoryNode` has already been cleaned up!

For now, Remus uses per-thread free lists when reclaiming memory, and favors
those lists on future allocations.  This is satisfactory for programs where all
threads have the same pattern of allocations.  Remember that general-purpose
allocation on RDMA is still an active research area!
