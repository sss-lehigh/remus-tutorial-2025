#pragma once

#include <atomic>

/// A lock-based sorted list-based set with wait-free contains
///
/// @param K The type for keys stored in this map
template <typename K, K DUMMY_KEY> class lazylist_omap {
  /// A simple test-and-test-and-set lock
  ///
  /// NB: The original code used a pthread_spinlock_t, which was causing
  ///     excessive latency.  This implementation is more faster, because it
  ///     uses XCHG without a function call.
  struct lock_t {
    std::atomic<bool> lock_ = {false}; // An atomic bool to serve as the lock

    /// Acquire the lock
    void acquire() {
      while (true) {
        if (!lock_.exchange(true))
          break;
        while (lock_.load()) {
        }
      }
    }

    /// Release the lock
    void release() { lock_.store(false); }
  };

  struct node_t : DESCRIPTOR::reclaimable_t {
    const K key;
    V val;
    std::atomic<node_t *> next;
    lock_t lock;
    node_t(K _key, V _val) : key(_key), val(_val), next(nullptr) {}
    ~node_t() {}
  };

public:
  node_t *head; // A pointer to the list head sentinel
  node_t *tail; // A pointer to the list tail sentinel

  /// Default construct a list by constructing and connecting two sentinel nodes
  lazylist_omap(DESCRIPTOR *me, auto *cfg) {
    head = new node_t(DUMMY_KEY, DUMMY_VAL);
    tail = new node_t(DUMMY_KEY, DUMMY_VAL);
    head->next = tail;
  }

private:
  bool is_marked_ref(uintptr_t ptr) { return ptr & 1; }

  uintptr_t unset_mark(uintptr_t ptr) { return ptr & (UINTPTR_MAX - 1); }

  uintptr_t set_mark(uintptr_t ptr) { return ptr | 1; }

  inline node_t *get_unmarked_ref(node_t *ptr) {
    return (node_t *)unset_mark((uintptr_t)ptr);
  }

  inline node_t *get_marked_ref(node_t *ptr) {
    return (node_t *)set_mark((uintptr_t)ptr);
  }

  /*
   * Checking that both curr and pred are both unmarked and that pred's next
   * pointer points to curr to verify that the entries are adjacent and present
   * in the list.
   */
  inline int parse_validate(node_t *pred, node_t *curr) {
    return (!is_marked_ref((uintptr_t)pred->next.load()) &&
            !is_marked_ref((uintptr_t)curr->next.load()) &&
            (pred->next == curr));
  }

public:
  bool get(DESCRIPTOR *me, const K &key, V &val) {
    node_t *curr = head;
    while (curr->key < key && curr != tail)
      curr = get_unmarked_ref(curr->next);
    V v = curr->val;
    auto res = ((curr != head) && (curr->key == key) &&
                !is_marked_ref((uintptr_t)curr->next.load()));
    if (res)
      val = v;
    return res;
  }

  // a failed validate is counted as an abort
  bool insert(DESCRIPTOR *me, const K &key, const V &val) {
    node_t *curr, *pred;
    int result, validated, notVal;
    while (true) {
      pred = head;
      curr = get_unmarked_ref(pred->next);
      while (curr->key < key && curr != tail) {
        pred = curr;
        curr = get_unmarked_ref(curr->next);
      }
      pred->lock.acquire();
      curr->lock.acquire();
      validated = parse_validate(pred, curr);
      notVal = (curr->key != key || curr == tail);
      result = (validated && notVal);
      if (result) {
        node_t *newnode = new node_t(key, val);
        newnode->next = curr;
        pred->next = newnode;
      }
      curr->lock.release();
      pred->lock.release();
      if (validated)
        return result;
    }
  }

  /*
   * Logically remove an element by setting a mark bit to 1
   * before removing it physically.
   *
   * NB. it is not safe to free the element after physical deletion as a
   * pre-empted find operation may currently be parsing the element.
   * TODO: must implement a stop-the-world garbage collector to correctly
   * free the memory.
   */
  // a failed validate is counted as an abort
  bool remove(DESCRIPTOR *me, const K &key) {
    node_t *pred, *curr;
    int result, validated, isVal;
    while (1) {
      pred = head;
      curr = get_unmarked_ref(pred->next);
      while (curr->key < key && curr != tail) {
        pred = curr;
        curr = get_unmarked_ref(curr->next);
      }
      pred->lock.acquire();
      curr->lock.acquire();
      validated = parse_validate(pred, curr);
      isVal = key == curr->key && curr != tail;
      result = validated && isVal;
      if (result) {
        curr->next = get_marked_ref(curr->next);
        pred->next = get_unmarked_ref(curr->next);
        me->reclaim(curr);
      }
      curr->lock.release();
      pred->lock.release();
      if (validated)
        return result;
    }
  }
};
