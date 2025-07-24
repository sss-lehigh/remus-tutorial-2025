---
outline: deep
---

# Building A Concurrent Data Structure

There was some code in the previous step of the tutorial that you probably did not like.  Remember this?

```c++
        compute_threads[0]->Write<uint64_t>(
            remus::rdma_ptr<uint64_t>(ptr.raw() +
                                      offsetof(SharedObject, values[i])),
            (uint64_t)0);
```

Remus can hide all of this complexity for you, resulting in code that looks a
lot more  like traditional shared memory code.

## The `Atomic` template

The `Atomic` template is Remus's RDMA equivalent to `std::atomic`.  It forces
you to be explicit about loading and storing from memory, but then it hides all
offset computation, giving a more natural programming experience.  Start by creating a new file called `lf_list.h`.

```c++
#pragma once

#include <memory>
#include <remus/remus.h>

using namespace remus;

template <typename K, typename V> class LockFreeList {

}
```

Inside of the list, define the type for nodes:

```c++
  struct Node {
    Atomic<K> key;
    Atomic<V> value;
    Atomic<Node *> next;

    void init(const K &k, const V &v,
              std::shared_ptr<ComputeThread> compute_thread) {
      key.store(k, compute_thread);
      value.store(v, compute_thread);
      next.store(nullptr, compute_thread);
    }
  };
```

In a shared memory program, you'd probably want `key` and `value` to be `const`,
but since they're in remote memory, here you'll use the `Atomic` template for
both of them, as well as for `next`.  Similarly, in a shared memory program,
you'd give `Node` a constructor, instead of an `init()` function.  While it's
possible to get the right behavior with a constructor and *placement new*, Remus
favors the use of separate `init` functions.  Later in this step of the
tutorial, you'll gain a better understanding of why (briefly: because within
`init()`, `this` is really an `rdma_ptr` created via `compute_thread::New()`.)

As you probably expect, the head pointer for this list will also be `Atomic`:

```c++
  Atomic<Node *> head;
```

Finally, you'll need to establish a special version of the `this` pointer:

```c++
  LockFreeList *This;
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
accessing the fields of the distributed object.  There are two mantras to keep in mind:

1. **Data is distributed, code is replicated**.
2. **`This` is for data, `this` is for code**.

## Creating The Data Structure

As you've probably guessed from the difference between `this` and `This`, in
order to get a friendly programming experience, you're going to need to
construct your data structure in a special way.  Remember that in C++, `new`
actually does two things: it allocates memory and it initializes that memory.  In Remus, we essentially need two constructors:

- `New` (capitalized) will allocate *distributed* memory and initialize it
- `new` (not capitalized), through the regular constructor, will allocate
  *local* memory and initialize it.

Here's how to do it in the LockFreeList:

```c++
  static rdma_ptr<LockFreeList>
  New(std::shared_ptr<ComputeThread> compute_thread) {
    auto list = compute_thread->New<LockFreeList>();
    list->head.store(nullptr, compute_thread);
    return rdma_ptr<LockFreeList>((uintptr_t)list);
  }

  LockFreeList(const remus::rdma_ptr<LockFreeList> &This)
      : This((LockFreeList *)((uintptr_t)This)) { }
```

In a few minutes, you'll write the code that calls these methods to construct a
list.  When you do, it will roughly follow these steps:

1. One thread will call `New` to allocate an object in shared memory.  Within
   `New`, `list` will be a remote address (e.g., its upper 16 bits specify a
   machine and segment, instead of being all zeroes).  `list->head` is the
   remote address of an `Atomic` field.  `store` is a method of `Atomic` that
   uses that remote address to perform a write to the distributed memory.  Finally, `list` is returned as an `rdma_ptr`.
2. Then every thread will use the `LockFreeList` constructor to make a local
   instance of the list where `This` is initialized.  All methods of the list
   will use `This` to interact with remote memory.

## Implementing Data Structure Methods

Now that `This` is initialized, the rest of the code is simple.  First, you
should implement the `get()` method:

```c++
  std::optional<V> get(const K &key, std::shared_ptr<ComputeThread> compute_thread) {
    Node *curr = This->head.load(compute_thread);
    while (curr) {
      if (curr->key.load(compute_thread) == key) {
        return curr->val.load(compute_thread);
      }
      if (curr->key.load(compute_thread) > key)
        return {};
      curr = curr->next.load(compute_thread);
    }
    return {};
  }
```

Other than the fact that your code uses `This->head` to get the head node,
instead of `this->head` (or just `head`), this code is identical to what you'd
write in many shared memory libraries.

Next, you should implement `insert()`:

```c++
  bool insert(const K &key, const V &value,
              std::shared_ptr<ComputeThread> compute_thread) {
    Node *new_node = compute_thread->New<Node>();
    new_node->init(key, value, compute_thread);
    while (true) {
      Node *prev = nullptr;
      Node *curr = This->head.load(compute_thread);
      // Find insertion point
      while (curr && curr->key.load(compute_thread) < key) {
        prev = curr;
        curr = curr->next.load(compute_thread);
      }
      if (curr && curr->key.load(compute_thread) == key) {
        compute_thread->Delete(new_node);
        return false; // Duplicate
      }
      new_node->next.store(curr, compute_thread);
      if (!prev) {
        // Insert at head
        if (This->head.compare_exchange_weak(curr, new_node, compute_thread)) {
          return true;
        }
      } else {
        if (prev->next.compare_exchange_weak(curr, new_node, compute_thread)) {
          return true;
        }
      }
      // CAS failed; retry
    }
  }
```

Finally, implement `remove()`:

```c++
  bool remove(const K &key, std::shared_ptr<ComputeThread> compute_thread) {
    while (true) {
      Node *prev = nullptr;
      Node *curr = This->head.load(compute_thread);
      // Find node to remove
      while (curr && curr->key.load(compute_thread) < key) {
        prev = curr;
        curr = curr->next.load(compute_thread);
      }
      if (!curr || curr->key.load(compute_thread) != key)
        return false; // Not found
      Node *next = curr->next.load(compute_thread);
      if (!prev) {
        // Remove head
        if (This->head.compare_exchange_weak(curr, next, compute_thread)) {
          compute_thread->Reclaim(curr);
          return true;
        }
      } else {
        if (prev->next.compare_exchange_weak(curr, next, compute_thread)) {
          compute_thread->Reclaim(curr);
          return true;
        }
      }
      // CAS failed; retry
    }
  }
```

## Old Notes

Now we'll create a lazy list.  Be sure to talk about the guarantees provided by
the allocator!

Timing and measuring need discussion

Also first-touch allocation / warm-up
