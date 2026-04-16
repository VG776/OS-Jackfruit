# Multi-Container Runtime

This repository implements a lightweight Linux container runtime in C with:

* a long-running parent supervisor in user space
* a bounded-buffer concurrent logging pipeline
* a kernel module that enforces soft and hard memory limits

Implementation files are in `boilerplate/`:

* `engine.c` — user-space runtime and supervisor
* `monitor.c` — kernel-space memory monitor
* `monitor_ioctl.h` — shared ioctl definitions
* `cpu_hog.c`, `io_pulse.c`, `memory_hog.c` — workloads
* `Makefile`

---

# 1. Team Information

* Name 1: V.Saatwik
* SRN 1: PES1UG24CS509
* Name 2: Varhit Gude 
* SRN 2: PES1UG24CS518

---

# 2. Build, Load, and Run Instructions

## Build

```bash
make
```

## Load kernel module

```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
```

## RootFS Setup (Ubuntu 25.10 ARM)

```bash
cd ~/Downloads/OS-Jackfruit

wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/aarch64/alpine-minirootfs-3.20.3-aarch64.tar.gz

rm -rf rootfs-base rootfs-alpha rootfs-beta
mkdir rootfs-base

tar -xzf alpine-minirootfs-3.20.3-aarch64.tar.gz -C rootfs-base

cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
```

## Copy workloads

```bash
cp boilerplate/cpu_hog rootfs-alpha/
cp boilerplate/memory_hog rootfs-alpha/

cp boilerplate/cpu_hog rootfs-beta/
cp boilerplate/io_pulse rootfs-beta/

chmod +x rootfs-alpha/*
chmod +x rootfs-beta/*
```

## Start supervisor

```bash
cd boilerplate
sudo ./engine supervisor ../rootfs-base
```

## Start containers

```bash
sudo ./engine start alpha ../rootfs-alpha "/cpu_hog 20"
sudo ./engine start beta ../rootfs-beta "/io_pulse 25 150"

sudo ./engine ps
sudo ./engine logs alpha
```

## Run container (blocks until exit)

```bash
sudo ./engine run test ../rootfs-alpha "/cpu_hog 10"
```

This command **blocks** until the container exits, then returns the exit code (or `128 + signal` if signaled).
The supervisor maintains the same logging pipeline for `run` containers as for `start` containers.

## Memory experiment

```bash
sudo sysctl kernel.dmesg_restrict=0

sudo ./engine start mem3 ../rootfs-alpha "/memory_hog 50 500" --soft-mib 40 --hard-mib 64

dmesg | tail -n 30
sudo ./engine ps
```

## Scheduler experiment

```bash
sudo ./engine start cslow ../rootfs-alpha "/cpu_hog 25" --nice 15
sudo ./engine start cfast ../rootfs-beta "/cpu_hog 25" --nice -5

sudo ./engine logs cslow
sudo ./engine logs cfast
```

## Cleanup

```bash
sudo ./engine stop cslow
sudo ./engine stop cfast

ps -eo pid,ppid,stat,cmd | grep engine

sudo rmmod monitor
```

---

# 3. Demo with Screenshots

## 3.1 Multi-container supervision

![multi](screenshots/s1_multi_container.png)

**Caption:**
Two containers are started under a single long-running supervisor. The output shows that the supervisor tracks both containers concurrently.

---

## 3.2 Metadata tracking

![meta](screenshots/s2_metadata.png)

**Caption:**
The engine ps output shows tracked metadata including container ID, PID, state, memory limits, and log path.

---

## 3.3 Bounded-buffer logging

![log](screenshots/s3_logging.png)

**Caption:**
The engine logs output shows container stdout captured through the pipe → bounded buffer → log file pipeline.

---

## 3.4 CLI and IPC

![cli](screenshots/s4_cli_ipc.png)

**Caption:**
CLI sends commands to supervisor via UNIX socket and receives responses, demonstrating IPC separation.

---

## 3.5 Soft-limit warning

![soft](screenshots/s5_soft_limit.png)

**Caption:**
Kernel monitor logs a soft-limit event when memory exceeds advisory threshold.

---

## 3.6 Hard-limit enforcement

![hard](screenshots/s6_hard_limit.png)

**Caption:**
Kernel enforces hard limit and kills container. Supervisor shows state as `hard_limit_killed`.

---

## 3.7 Scheduler experiment

![sched](screenshots/s7_scheduler.png)

**Caption:**
CPU workloads with different nice values demonstrate scheduling priority differences.

---

## 3.8 Clean teardown

![cleanup](screenshots/s8_cleanup.png)

**Caption:**
Containers are stopped and no zombie processes remain. Only supervisor processes exist.

---

# 4. Engineering Analysis

## 4.1 Isolation Mechanisms

This runtime uses Linux namespaces (PID, UTS, mount) and `chroot()` to isolate containers.
Each container has its own process space, hostname, and filesystem view.

However, the kernel is shared. This allows the kernel module to enforce memory limits globally.

---

## 4.2 Supervisor and Lifecycle

A persistent supervisor manages:

* container creation
* metadata
* logging
* reaping

Containers are tracked centrally, ensuring correct state transitions and cleanup.

**Container Termination Classification:**

The `stop_requested` flag in the container record enables precise classification of how each container exited:

1. **Graceful stop (`CONTAINER_STOPPED`):** When `stop` command is issued, the supervisor sets `stop_requested=1` and sends `SIGTERM`. If the container exits (normally or by any signal) within the grace period, the exit is classified as `stopped` because `stop_requested` was set. If the container doesn't exit after grace attempts, `SIGKILL` is sent (with `stop_requested` still set), and the exit is still classified as `stopped` because the stop was explicitly requested.

2. **Hard-limit kill (`CONTAINER_KILLED`):** The kernel monitor sends `SIGKILL` when a container exceeds its hard limit. The exit status shows `exit_signal == SIGKILL`, but `stop_requested==0` (never set by the supervisor), so the exit is classified as `hard_limit_killed`. This attribution allows the README and logs to distinguish enforced resource limits from administrative stops.

3. **Normal exit (`CONTAINER_EXITED`):** If a container exits on its own (either normally with an exit code or by receiving an unsolicited signal like `SIGSEGV`), and `stop_requested==0`, the exit is classified as `exited`. This covers both expected completions and crashes.

**Grace Period Handling:**

When `stop` is called, `ensure_stopped()` first sends `SIGTERM` and polls with `usleep()` to allow graceful shutdown. After `STOP_GRACE_ATTEMPTS` polls (~2 seconds total), if the container hasn't exited, `SIGKILL` is issued. Both paths maintain `stop_requested=1` so the final state reflects an administrative stop, not a kernel enforcement.

---

## 4.3 IPC and Synchronization

Two IPC paths:

* pipes → logging (container stdout/stderr to bounded buffer)
* UNIX socket → control (CLI to supervisor)

### Bounded-Buffer Logging Design

**Synchronization:**

* **Mutex (`pthread_mutex_t`)** protects the circular buffer state (head, tail, count)
* **Condition variables** (`not_empty`, `not_full`) enable efficient producer-consumer wake-up without busy-waiting

**Race Conditions Without Synchronization:**

Without the mutex, concurrent reads and writes to `head`, `tail`, and `count` would cause data corruption:
- Producer and consumer might both modify `tail` and `head` simultaneously, losing updates
- Lost updates to `count` could cause the buffer to incorrectly report free space
- Producer could write over data the consumer hasn't yet read
- Consumer could read uninitialized or stale data

**Deadlock Avoidance:**

The design prevents deadlock through:
1. **Lock ordering:** All code paths acquire the mutex once and release it before re-acquiring. No nested lock attempts.
2. **Condition variable signaling:** Producers signal `not_empty` to wake consumers; consumers signal `not_full` to wake producers. Both directions are covered, preventing permanent waits.
3. **Graceful shutdown:** `bounded_buffer_begin_shutdown()` broadcasts on both condition variables before any thread exits, ensuring all waiters wake up and check the shutdown flag.

**Correctness Properties Demonstrated:**

1. **No log loss on abrupt container exit:**  
   The producer thread reads from the container's pipe until EOF (when the container exits). All data is inserted into the bounded buffer via `bounded_buffer_push()`. Even if a container is killed abruptly, buffered data remains in the queue until the consumer thread (`logging_thread`) flushes it to disk. The consumer only exits when `bounded_buffer_pop()` returns -1 (after shutdown), ensuring all queued entries are written.

2. **Buffer does not deadlock when full:**  
   When the buffer is full (`count == LOG_BUFFER_CAPACITY`), a producer blocks on `pthread_cond_wait(&not_full, ...)`. The consumer, running in a separate thread, removes items and calls `pthread_cond_signal(&not_full)`. This wakes the producer to retry. If multiple producers wait, `pthread_cond_signal()` is sufficient because the producer loop re-checks the condition. The supervisor creates only one producer per container, making contention low.

3. **Threads terminate cleanly:**  
   `bounded_buffer_begin_shutdown()` sets `shutting_down=1` and broadcasts on both condition variables. Any producer or consumer currently blocked wakes up, observes `shutting_down=1` in their loop condition, and returns immediately. The supervisor joins all producer threads in `free_container_record()` before exiting. The logging thread is joined in `supervisor_main()` after signaling buffer shutdown, ensuring it has flushed all remaining entries to disk files before termination.

**Lock-Protected Metadata Access:**

Container metadata (PID, state, exit code, limits) is protected by `pthread_mutex_t lock` in the container record and `metadata_lock` in the supervisor context to prevent races during concurrent CLI queries and signal handling.

---

## 4.4 Memory Enforcement

* RSS used as memory metric
* soft limit → warning
* hard limit → SIGKILL

Kernel enforcement ensures correctness and avoids user-space races.

---

## 4.5 Scheduling Behavior

Experiments show:

* lower nice → higher priority
* CPU-bound vs IO-bound differences

Linux scheduler balances fairness and responsiveness.

---

# 5. Design Decisions and Tradeoffs

## Isolation

* Used `chroot` instead of `pivot_root`
* Simpler but slightly weaker isolation

## Supervisor

* Single supervisor simplifies control
* Adds concurrency complexity

## Logging

* bounded buffer prevents blocking
* adds synchronization overhead

## Kernel Monitor

* linked list used
* simpler but O(n) lookup

---

# 6. Scheduler Experiment Results

## CPU vs CPU with Different Priorities

Two containers run the same CPU-bound workload with different `nice` values:

```bash
sudo ./engine start cslow ../rootfs-alpha "/cpu_hog 25" --nice 15
sudo ./engine start cfast ../rootfs-beta "/cpu_hog 25" --nice -5

sudo ./engine logs cslow
sudo ./engine logs cfast
```

Observed execution profiles:

| Container | PID   | Nice | Duration (log updates) | Completion |
|-----------|-------|------|------------------------|------------|
| cfast     | 54584 | -5   | ~25 iterations         | earlier    |
| cslow     | 54586 | +15  | ~25 iterations         | later      |

**Evidence:** The scheduler grants more CPU time to the higher-priority (`nice -5`) container, allowing it to complete iterations faster. The lower-priority container (`nice +15`) is preempted more frequently and completes iterations more slowly, demonstrating fairness with priority weighting.

## CPU vs IO

| Container | Type | Behavior                      | Log Impact           |
|-----------|------|-------------------------------|----------------------|
| cpu_hog   | CPU  | continuous computation       | steady output rate   |
| io_pulse  | IO   | burst I/O + sleep pattern     | bursty log intervals |

The I/O-bound workload shows periods of inactivity (sleep), while the CPU-bound workload generates logs continuously, illustrating how the scheduler differs in handling workload patterns.

Scheduler prioritizes:

* responsiveness for IO
* fairness for CPU

---

# 7. Ubuntu 25.10 Compatibility

This project was developed and tested on **Ubuntu 25.10 (ARM64 architecture)** inside a virtual machine environment. During development, several system-specific issues were encountered and resolved as follows:

---

### 7.1 Architecture Mismatch (Exec Format Error)

Initially, container workloads failed with:

```bash
exec: Exec format error
```

**Cause:**
The root filesystem and/or binaries were not compatible with the system architecture.

**Fix:**
An ARM64-compatible Alpine root filesystem was used:

```bash
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/aarch64/alpine-minirootfs-3.20.3-aarch64.tar.gz
```

All workload binaries (`cpu_hog`, `memory_hog`, `io_pulse`) were compiled inside the same ARM environment and copied into the rootfs.

---

### 7.2 Missing Binaries Inside Container (Exit Code 127)

Containers initially exited with:

```bash
status=127
sh: /cpu_hog: not found
```

**Cause:**
The workload binaries were not present inside the container root filesystem or were not executable.

**Fix:**
Binaries were explicitly copied and permissions fixed:

```bash
cp boilerplate/cpu_hog rootfs-alpha/
cp boilerplate/io_pulse rootfs-beta/

chmod +x rootfs-alpha/*
chmod +x rootfs-beta/*
```

After this, containers executed workloads successfully.

---

### 7.3 Supervisor Not Running (IPC Failure)

Error observed:

```bash
connect: No such file or directory
```

**Cause:**
The CLI attempted to communicate with the supervisor before it was started.

**Fix:**
Ensure supervisor is running before issuing commands:

```bash
sudo ./engine supervisor ../rootfs-base
```

This initializes the UNIX domain socket used for IPC.

---

### 7.4 dmesg Access Restriction

Kernel logs initially failed with:

```bash
dmesg: read kernel buffer failed: Operation not permitted
```

**Cause:**
Ubuntu restricts access to kernel logs by default.

**Fix:**
Temporarily disable restriction:

```bash
sudo sysctl kernel.dmesg_restrict=0
```

This allowed observation of soft and hard memory limit enforcement.

---

### 7.5 Root-Owned Files and Permission Issues

Certain directories (e.g., `logs/`) were created with root ownership due to use of `sudo`.

**Issue:**
Normal user could not delete or modify these files.

**Fix:**
Either remove using sudo:

```bash
sudo rm -rf boilerplate/logs
```

Or reset ownership:

```bash
sudo chown -R seed:seed boilerplate/
```

---

### 7.6 Root Filesystem Placement

The root filesystem directories (`rootfs-alpha`, `rootfs-beta`, etc.) were initially created both inside and outside the `boilerplate/` directory.

**Resolution:**
A consistent structure was adopted:

* rootfs directories placed at project root
* engine executed from `boilerplate/` using relative paths

---