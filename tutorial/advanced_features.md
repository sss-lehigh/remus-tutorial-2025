---
outline: deep
---

# Advanced Remus Features

In the landing page for this tutorial, you might have noticed a link to the
Remus API documentation.  If you dig through it, or look at the Remus code, you
will discover some performance-enhancing features that were not discussed in
this tutorial.  They include:

- Techniques for avoiding copying of data when reading from and writing to the
  distributed memory (zero copy).
- Support for asynchronous reading and writing, where a thread can issue a
  remote memory operation, continue executing, and periodically poll to see if
  the memory operation completed.
- Lightweight threads for increasing concurrency without incurring high context
  switching overheads (fibers) (**coming soon**).

If time permits, you should try to use some of these features, along with some
good old fashioned hand-tuning of your code.  You should be able to increase
throughput by at least a factor of two, and maybe more!
