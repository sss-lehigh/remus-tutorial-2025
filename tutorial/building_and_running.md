---
outline: deep
---

# Building And Running A Remus Program

Running a distributed program is always challenging.  It requires you to launch
separate processes on separate machines, and to pass the right configuration
into those processes so that they know how to interact with each other.

In this step of the Remus tutorial, you'll learn how to build a simple Remus
program and run it on several CloudLab machines.

## Working In The Remus Repository

In the previous step, you checked out the `remus` repository from GitHub and set
up a Docker container for building the code.  In this step, you will add and
edit files in the `benchmark` subfolder.

In that folder, you'll see a `CMakeLists.txt` file.  It describes programs to
build, usually using just two lines per program:

```cmake
add_executable(ds_launch ds_launch.cc)
target_link_libraries(ds_launch PRIVATE rdma)
```

This corresponds to a simple program design, where a program consists of one
`.cc` file, along with however many `.h` files it needs to include.  It is
possible to build more complex programs with Remus, but in this tutorial, you'll
want to stick to this approach.

To get started, make a new file called `benchmark/hello.cc`:

```c++
#include <iostream>

int main(int, char**) {
    std::cout << "Hello World" << std::endl;
}
```

Then add this to the `CMakeLists.txt` file:

```cmake
# Build the "hello" executable
add_executable(hello hello.cc)
target_link_libraries(hello PRIVATE rdma)
```

In order to build your new program, type `make` in the **parent** of the
`benchmark` folder.  You should see output like this:

:::warning
The following screenshot isn't correct, because it wasn't generated inside of the container.
:::

```text
-- Using standard 20
-- Using LOG_LEVEL=RELEASE
-- Configuring done (0.0s)
-- Generating done (0.0s)
-- Build files have been written to: /home/sss/remus/remus/build
gmake[1]: Entering directory '/home/sss/remus/remus/build'
gmake[2]: Entering directory '/home/sss/remus/remus/build'
gmake[3]: Entering directory '/home/sss/remus/remus/build'
gmake[3]: Entering directory '/home/sss/remus/remus/build'
gmake[3]: Entering directory '/home/sss/remus/remus/build'
gmake[3]: Entering directory '/home/sss/remus/remus/build'
gmake[3]: Entering directory '/home/sss/remus/remus/build'
gmake[3]: Entering directory '/home/sss/remus/remus/build'
gmake[3]: Entering directory '/home/sss/remus/remus/build'
gmake[3]: Entering directory '/home/sss/remus/remus/build'
gmake[3]: Leaving directory '/home/sss/remus/remus/build'
gmake[3]: Leaving directory '/home/sss/remus/remus/build'
gmake[3]: Leaving directory '/home/sss/remus/remus/build'
gmake[3]: Leaving directory '/home/sss/remus/remus/build'
gmake[3]: Leaving directory '/home/sss/remus/remus/build'
gmake[3]: Leaving directory '/home/sss/remus/remus/build'
gmake[3]: Leaving directory '/home/sss/remus/remus/build'
gmake[3]: Leaving directory '/home/sss/remus/remus/build'
[ 10%] Built target root_test
[ 20%] Built target alloc_pol_1024
[ 30%] Built target perftest_1024
[ 40%] Built target helloworld_test
gmake[3]: Entering directory '/home/sss/remus/remus/build'
gmake[3]: Entering directory '/home/sss/remus/remus/build'
[ 60%] Built target alloc_pol_8
[ 60%] Built target write_test
[ 70%] Built target read_test
[ 80%] Built target perftest_8
gmake[3]: Leaving directory '/home/sss/remus/remus/build'
gmake[3]: Entering directory '/home/sss/remus/remus/build'
gmake[3]: Leaving directory '/home/sss/remus/remus/build'
[ 85%] Building CXX object benchmark/CMakeFiles/hello.dir/hello.cc.o
[ 95%] Built target ds_launch
[100%] Linking CXX executable hello
gmake[3]: Leaving directory '/home/sss/remus/remus/build'
[100%] Built target hello
gmake[2]: Leaving directory '/home/sss/remus/remus/build'
gmake[1]: Leaving directory '/home/sss/remus/remus/build'
```

The program will be built in the `build/benchmark` subfolder.  You can run it on
your local machine by typing `./build/benchmark/hello`.

## Managing Command-Line Arguments

When running a Remus program, the process on each node needs a lot of
configuration information, including:

- The name of the machine where it is running
- The names of the machines that are serving as memory nodes
- How many memory segments are being hosted on each memory node, and how big
  they are
- The port on which to connect to those memory nodes
- The names of the machines that are serving as compute nodes
- How many threads should run on each compute node
- How to configure the queue pairs from each compute node to each memory node
- Other resource management details

:::tip
These configuration parameters will be explained in more detail later.  If they
don't make sense right now, it's OK.
:::

`remus::ArgMap` helps with providing and managing this information. You can
`import` one or more lists of `remus::Arg` objects into an `ArgMap`, and then
`parse` the `ArgMap`.  This will terminate the program if any required arguments
are not provided, and otherwise leave you with an unordered map that your code
can query to get the arguments to the program.

We provide a list of arguments called `remus::ARGS`, which describes all of the
standard Remus configuration information.  To test it out, replace `hello.cc`:

```c++
#include <memory>
#include <remus/remus.h>

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
}
```

If you build and re-run `build/benchmark/hello`, you should see an error
message:

```text
REMUS::DEBUG is false
hello
  --alloc-pol: How should ComputeThreads pick Segments for allocation: RAND, GLOBAL-RR, GLOBAL-MOD, LOCAL-RR, LOCAL-MOD
  --cn-ops-per-thread: The maximum number of concurrent messages that a thread can issue without waiting on a completion.
  --cn-thread-bufsz: The log_2 of the size of the buffer to allocate to each compute thread.
  --cn-threads: The number of threads to run on each compute node
  --cn-wrs-per-seq: The number of sequential operations that a thread can perform concurrently.
  --first-cn-id: The node-id of the first node that performs computations.
  --first-mn-id: The node-id of the first node that hosts memory segments.
  --help: Print this help message
  --last-cn-id: The node-id of the last node that performs computations.
  --last-mn-id: The node-id of the last node that hosts memory segments.
  --mn-port: The port that memory nodes should use to wait for connections during the initialization phase.
  --node-id: A numerical identifier for this node.
  --qp-lanes: Each compute node should have qp-lanes connections to each memory node.
  --qp-sched-pol: How to choose which qp to use: RAND, RR, or MOD
  --seg-size: The size of each remotely-accessible memory segment on each memory node will be 2^{seg-size}.
  --segs-per-mn: The number of remotely-accessible memory segments on each memory node.
terminate called after throwing an instance of 'std::runtime_error'
  what():  Error: `--cn-threads` is required
Aborted (core dumped)
```

Let's try to provide reasonable values for all of the required arguments:

```bash
./build/benchmark/hello --seg-size 29 --segs-per-mn 2 --first-cn-id 2 --last-cn-id 2 --first-mn-id 0 --last-mn-id 1 --qp-lanes 2 --qp-sched-pol RR --mn-port 33330 --cn-threads 2 --cn-ops-per-thread 4 --cn-thread-bufsz 20 --alloc-pol GLOBAL-RR --node-id 0
```

This results in the following output from `report_config`, which is just a dump
of the provided configuration, in CSV format:

```text
REMUS::DEBUG is false
hello (--alloc-pol --cn-ops-per-thread --cn-thread-bufsz --cn-threads --cn-wrs-per-seq --first-cn-id --first-mn-id --help --last-cn-id --last-mn-id --mn-port --node-id --qp-lanes --qp-sched-pol --seg-size --segs-per-mn ), GLOBAL-RR, 4, 20, 2, 16, 2, 0, false, 3, 1, 33330, 0, 2, RR, 29, 2
```

## Running On CloudLab

Now it's time to run the program on CloudLab, instead of on your local machine.
Participants in this tutorial should have been given a CloudLab allocation
consisting of several "r320" machine names of the form `apt123`.  Names prefixed
with `apt` are public names, accessible from anywhere in the world.  However,
from inside of CloudLab, machines can be accessed using 0-indexed names:
`node0`, `node1`, etc.  You'll see how this works in the next tutorial step. For
now, the goal is just to get a program running on several machines at once.

This tutorial assumes that you've already provided your ssh key to CloudLab.
That key is necessary for connecting via `ssh` and `scp`.  Now it's time to do
a one-time installation of important libraries on the machines you've been
allocated.

You can do all of this through the `cl.sh` script, but it needs some
configuration.  To avoid checking your configuration into the repository by
mistake, you should make use of the `local/` folder.  If it doesn't exist,
create it and then copy the `cl.config` template into it:

```bash
mkdir -p local
cp cl.config local/my_config
```

Assuming that you've been allocated three machines with the names `apt030`,
`apt132`, and `apt028`, you should now update your config file accordingly.  Note that the order of the machine names matters!

```bash
domain=apt.emulab.net
machines=(apt030 apt132 apt028)
user=your_cloudlab_username
```

Now you should be able to use `cl.sh` to configure all of the machines:

```bash
./cl.sh local/my_config install-deps
```

This script uses `screen` to install dependencies on all of the machines in
parallel.  While it's running, you can use `ctrl-j` as the command character for
navigating among the open screens (e.g., `ctrl-j 1` to switch to the second
screen).  This can be useful if the software update process requires any input.

Once the installation has finished, all screens will close, and then the status
of the RDMA cards on the CloudLab machines will be reported.  It typically looks
like this:

```text
hca_id: mlx4_0
        transport:                      InfiniBand (0)
        fw_ver:                         2.42.5000
        node_guid:                      f452:1403:0015:6fc0
        sys_image_guid:                 f452:1403:0015:6fc3
        vendor_id:                      0x02c9
        vendor_part_id:                 4099
        hw_ver:                         0x1
        board_id:                       MT_1090120019
        phys_port_cnt:                  2
                port:   1
                        state:                  PORT_ACTIVE (4)
                        max_mtu:                4096 (5)
                        active_mtu:             4096 (5)
                        sm_lid:                 203
                        port_lid:               108
                        port_lmc:               0x00
                        link_layer:             InfiniBand

                port:   2
                        state:                  PORT_ACTIVE (4)
                        max_mtu:                4096 (5)
                        active_mtu:             1024 (3)
                        sm_lid:                 0
                        port_lid:               0
                        port_lmc:               0x00
                        link_layer:             Ethernet
```

There's one more step before you can run your program on those machines: `cl.sh`
needs to know the arguments to provide to the program.  Copy the experiment
configuration template:

```bash
cp cl.experiment local/my_experiment
```

Then edit it.  Note that in the example below, the `--node-id` flag is not
included, but all other arguments are provided:

```bash
exefile=build/benchmark/hello
experiment_args="--seg-size 29 --segs-per-mn 2 --first-cn-id 2 --last-cn-id 2 --first-mn-id 0 --last-mn-id 1 --qp-lanes 2 --qp-sched-pol RR --mn-port 33330 --cn-threads 2 --cn-ops-per-thread 4 --cn-thread-bufsz 20 --alloc-pol GLOBAL-RR"
```

Build the program, transfer it to CloudLab, and run it on all machines:

```bash
./cl.sh local/my_config build-run local/my_experiment
```

When the program terminates, screen will remain open, so you can see each
program's output.  To close a screen, type `exit` or press `ctrl-d`.

If you do not need to re-build and re-upload the executable before running it
again, you can replace `build-run` with `run`:

```bash
./cl.sh local/my_config run local/my_experiment
```

:::tip
Coupling this command with different experiment configuration files is useful
when performing batches of experiments.
:::
