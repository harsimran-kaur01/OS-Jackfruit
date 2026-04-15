# 🧩 Container Runtime Design – Systems Analysis

## 👩‍💻 Authors
- Harsimran Kaur — PES1UG24CS187
- Gitanjali A — PES1UG24CS170  

  
## 1. Isolation Mechanisms

Our runtime achieves isolation primarily using Linux namespaces and filesystem root switching.

### 🔒 Process Isolation (PID Namespace)
- A PID namespace ensures that processes inside the container have their own PID numbering  
- The container’s init process appears as PID 1 inside the namespace  
- Processes outside the namespace cannot directly see or interact with container processes  

### 🖥️ System Identity Isolation (UTS Namespace)
- The UTS namespace isolates hostname and domain name  
- Each container can have its own hostname without affecting the host  

### 📁 Filesystem Isolation (Mount Namespace + chroot/pivot_root)
- A mount namespace provides a separate view of the filesystem hierarchy  
- Changes to mounts inside the container do not affect the host  

#### chroot vs pivot_root

**chroot:**
- Changes the apparent root directory for a process  
- Does not fully isolate mount points; escape is possible in some cases  

**pivot_root:**
- Swaps the current root filesystem with a new one  
- More robust for containers; used in real runtimes  

### ⚠️ What the Kernel Still Shares
Even with namespaces:
- Kernel is shared across all containers  
- Shared resources include:
  - CPU scheduler  
  - Memory management subsystem  
  - Kernel modules  
  - Network stack (unless network namespaces are used)  

👉 This is why containers are lightweight but less isolated than full virtual machines.

---

## 2. Supervisor and Process Lifecycle

A long-running parent supervisor process plays a critical role in managing containers.

### 👨‍👩‍👧 Process Hierarchy
- The supervisor acts as the parent process for all containers  
- Containers are created using `fork()` / `clone()`  

### 🔁 Lifecycle Management
Creation → Execution → Monitoring → Cleanup  

### 🧹 Process Reaping
- When a child exits, it becomes a zombie until reaped  
- The supervisor calls `wait()` / `waitpid()` to:
  - Collect exit status  
  - Prevent zombie accumulation  

### 📊 Metadata Tracking
The supervisor maintains:
- Container PID  
- Status (running/stopped)  
- Resource usage  
- Logs  

### 📡 Signal Handling
- Signals like `SIGTERM`, `SIGKILL` are forwarded from supervisor to container  
- Ensures controlled shutdown  

### ✅ Why Supervisor is Important
- Central control point  
- Prevents orphan/zombie processes  
- Enables clean lifecycle transitions  
- Allows monitoring and logging  

---

## 3. IPC, Threads, and Synchronization

Our system uses multiple IPC mechanisms and a bounded-buffer logging system.

### 🔗 IPC Mechanisms Used
**Pipes**
- For parent-child communication  
- Simple, unidirectional data flow  

**Shared Memory**
- Fast communication between threads/processes  
- Used for logging buffer  

---

### 📦 Bounded Buffer Logging Design
We use a producer-consumer model:
- Producers: container processes generating logs  
- Consumer: logging thread writing to file/stdout  

---

### ⚠️ Race Conditions

**Shared Data Structures:**
- Log buffer (queue)  
- Read/write indices  
- Buffer count  

**Possible Issues:**
- Two producers writing simultaneously → data corruption  
- Consumer reading while producer writes → inconsistent data  
- Buffer overflow/underflow  

---

### 🔐 Synchronization Choices

| Problem | Solution | Why |
|--------|---------|-----|
| Mutual exclusion | Mutex | Ensures only one thread accesses buffer at a time |
| Buffer full/empty | Condition Variables | Efficient waiting (no busy-waiting) |
| Producer-consumer coordination | Semaphore (optional) | Tracks available slots/items |

### Why not spinlocks?
- Waste CPU cycles under contention  
- Not ideal for user-space blocking workloads  

### ✅ Final Design
- Mutex → protects critical section  
- Condition variables → handle waiting efficiently  

---

## 4. Memory Management and Enforcement

### 📊 What RSS Measures
**RSS (Resident Set Size):**
- Physical memory currently used by a process  

**Includes:**
- Code  
- Stack  
- Heap  

**Excludes:**
- Swapped-out pages  
- Shared pages (may be partially counted)  

---

### ❗ What RSS Does NOT Measure
- Total virtual memory  
- Memory mapped but unused regions  
- Disk-backed swapped memory  

---

### ⚖️ Soft vs Hard Limits

| Type | Behavior |
|------|----------|
| Soft Limit | Advisory; can be exceeded temporarily |
| Hard Limit | Strict; exceeding leads to termination |

**Why both?**
- Soft limits allow flexibility under low contention  
- Hard limits enforce strict resource control  

---

### 🧠 Why Enforcement Must Be in Kernel Space
User-space enforcement alone is insufficient because:
- Processes can ignore limits  
- No control over actual memory allocation  
- Cannot intercept low-level allocation (e.g., page faults)  

Kernel-space enforcement:
- Controls physical memory allocation  
- Can terminate processes (OOM killer)  
- Enforces fairness globally  

---

## 5. Scheduling Behavior

Our experiments reveal how Linux scheduling impacts workload behavior.

### ⚙️ Observations

#### 🧮 CPU-bound Workloads
- Fairly distributed CPU time  
- Scheduler ensures fairness  
- Processes get near-equal execution slices  

#### ⏱️ I/O-bound Workloads
- Higher responsiveness  
- Tasks frequently yield CPU  
- Scheduler prioritizes them for better latency  

---

### 🎯 Scheduling Goals

#### 1. Fairness
- Achieved via Completely Fair Scheduler (CFS)  
- Each process gets proportional CPU time  

#### 2. Responsiveness
- I/O-bound tasks prioritized  
- Lower latency for interactive processes  

#### 3. Throughput
- CPU utilization remains high  
- Minimal idle time  

---

### 🔍 Interpretation of Results
- In mixed workloads:
  - CPU-bound tasks do not starve others  
  - I/O-bound tasks complete faster due to frequent scheduling  

- This confirms Linux balances:
  - Throughput  
  - Fairness  
  - Responsiveness  

---
# Container Engine – Build, Load, and Run Instructions

## 1. Build the Project

Navigate to the boilerplate directory and compile the project:

```bash
cd boilerplate
make
```

---

## 2. Load Kernel Module

Insert the kernel module and verify the device:

```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
```

---

## 3. Start Supervisor

Launch the container supervisor using the base root filesystem:

```bash
sudo ./engine supervisor ./rootfs-base
```

---

## 4. Prepare Containers

Create container root filesystems by copying the base:

```bash
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

---

## 5. Start Containers

Run containers with specified workloads:

```bash
sudo ./engine start alpha ./rootfs-alpha /cpu_hog
sudo ./engine start beta ./rootfs-beta /io_pulse
```

---

## 6. Inspect Containers

Check running containers and view logs:

```bash
sudo ./engine ps
sudo ./engine logs alpha
```

---

## 7. Stop Containers and Cleanup

Stop containers and remove the kernel module:

```bash
sudo ./engine stop alpha
sudo ./engine stop beta
sudo rmmod monitor
```

---

## Notes

* Ensure you have root privileges (`sudo`) for all commands interacting with the kernel module and engine.
* Verify that `monitor.ko` is built successfully before loading.
* The `/dev/container_monitor` device should exist after inserting the module.
* Logs can help debug container execution issues.

## ✅ Summary

This runtime demonstrates:
- Strong isolation using namespaces and filesystem techniques  
- Robust lifecycle management via a supervisor  
- Safe concurrency using proper synchronization primitives  
- Kernel-enforced memory control for reliability  
- Predictable scheduling aligned with Linux design goals
  
