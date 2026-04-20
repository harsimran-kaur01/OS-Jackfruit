## OS-JACK-FRUIT
A lightweight Linux container runtime written in C, featuring a long-running supervisor and a kernel-space memory monitor. This project demonstrates core operating system concepts such as namespaces, process isolation, scheduling, IPC, and kernel-level enforcement.

## Features
```
Namespace-based container isolation
Kernel-space memory monitoring
Supervisor for lifecycle management
IPC using pipes and UNIX sockets
Multithreaded logging system
Scheduling experiments using Linux CFS
```

## Team Information
```
Name	        SRN
Harsimran Kaur	PES1UG24CS187
Gitanjali A     PES1UG24CS170
Prerequisites
Ubuntu 22.04 / 24.04 (VM or bare metal)
Secure Boot OFF (required for kernel module)
Linux kernel headers installed
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
Setup Instructions
git clone https://github.com/<your-username>/OS-Jackfruit.git
cd OS-Jackfruit

# Extract root filesystems
sudo tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
sudo tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-alpha
sudo tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-beta
Build
cd boilerplate
sudo make

# Copy workloads into containers
cp cpu_hog memory_hog io_pulse ../rootfs-alpha/
cp cpu_hog memory_hog io_pulse ../rootfs-beta/
Load Kernel Module
sudo insmod monitor.ko
ls -l /dev/container_monitor
dmesg | tail
Ensure:

Device /dev/container_monitor exists
"Module loaded" appears in logs
Running the System
Terminal 1 — Start Supervisor
sudo ./engine supervisor ../rootfs-base
Terminal 2 — Manage Containers
# Start containers
sudo ./engine start alpha /path/to/rootfs-alpha /bin/sleep 100
sudo ./engine start beta /path/to/rootfs-beta /bin/sleep 100

# View status
sudo ./engine ps

# View logs
sudo ./engine logs alpha

# Stop container
sudo ./engine stop alpha
Run in Foreground Mode
sudo ./engine run test /path/to/rootfs-alpha /bin/ls
Cleanup
sudo ./engine stop alpha
sudo ./engine stop beta

# Stop supervisor
Ctrl + C

sudo rmmod monitor
dmesg | tail
Ensure:

"Module unloaded" appears
Demo Screenshots
Place all images inside a folder named screenshots/.

Feature	Screenshot
Multi-container supervision	
Metadata tracking	
Bounded-buffer logging	
CLI and IPC	
Soft-limit warning	

Hard-limit enforcement	
Scheduling experiment	
Clean teardown	

Engineering Analysis
Isolation Mechanisms
Uses clone() with:

CLONE_NEWPID
CLONE_NEWUTS
CLONE_NEWNS
chroot() restricts filesystem

/proc mounted inside container

Host kernel is shared across containers

Supervisor & Process Lifecycle
Prevents zombie processes using:

waitpid(-1, WNOHANG)
Tracks containers using linked list:

Container ID
PID
State
Memory limits
IPC, Threads & Synchronization
Pipe per container captures stdout/stderr
UNIX domain socket handles CLI communication
Bounded buffer design:

Mutex for mutual exclusion

Condition variables:

not_full
not_empty
Memory Management
Uses RSS (Resident Set Size)
Soft limit → warning (logged once)
Hard limit → process killed (SIGKILL)
Implemented in kernel space using a timer for reliable enforcement

Scheduling (Linux CFS)
Experiment 1 — CPU-bound Tasks
Container	Nice	Completion Time
High	-5	~9.120 sec
Low	+10	~9.416 sec
Higher priority results in more CPU share

Experiment 2 — CPU vs I/O
I/O-bound process yields CPU frequently
Gets priority boost on wakeup
Maintains responsiveness
Design Decisions & Tradeoffs
Component	Decision	Tradeoff	Reason
Namespace isolation	No network namespace	Limited isolation	Simpler design
Supervisor	Single-threaded loop	Serialized requests	Easier implementation
IPC	Pipe + UNIX socket	Extra threads	Separation of concerns
Kernel monitor	Mutex	No IRQ usage	Runs in process context
Scheduling	nice values	Only visible under load	Demonstrates CFS
Key Highlights
Built entirely in C
Demonstrates real OS concepts
Kernel and user-space integration
Efficient logging and IPC system
Practical scheduling experiments
Notes
Run all commands with sudo
Ensure Secure Boot is disabled
Verify correct rootfs paths
Kernel module must be loaded before running containers
Project Structure
OS-Jackfruit/
│── boilerplate/
│── rootfs-base/
│── rootfs-alpha/
│── rootfs-beta/
│── screenshots/
│── engine
│── monitor.ko
How to Add Screenshots
Create folder:
mkdir screenshots
Add images:
os_1.png
os_2.png
...
Commit:
git add .
git commit -m "Added screenshots"
git push
Conclusion
OS-JACK-FRUIT is a compact demonstration of how container runtimes work internally. It integrates Linux namespaces, kernel monitoring, scheduling, and IPC into a cohesive system, making it a strong educational and experimental platform for operating systems.



## Design and Implementation Overview

The system is designed using Linux namespaces such as PID, UTS, and Mount to provide process, hostname, and filesystem isolation, while each container is further isolated using chroot to give it an independent root filesystem. A central supervisor manages all containers by handling their lifecycle operations such as start, stop, and cleanup of processes to prevent zombie states. Communication between the CLI and supervisor is implemented using UNIX domain sockets, while logging is handled through pipes and stored in separate log files for each container. Memory management is enforced through soft and hard limits, where the soft limit triggers warnings and the hard limit results in process termination, with monitoring performed via a kernel module. CPU scheduling is demonstrated using Linux nice values, where lower nice values receive higher CPU priority and higher nice values receive reduced CPU access. The design choices prioritize simplicity and clarity, using chroot for lightweight filesystem isolation, a single supervisor for centralized control, pipes for efficient logging, UNIX sockets for fast inter-process communication, and nice values to demonstrate basic scheduling behavior. In experiments, a container with nice value 0 received nearly 100% CPU usage, while a container with nice value 10 received significantly less CPU time (around 60%), showing that lower nice values result in higher CPU allocation.

## Engineering Analysis

This section explains the operating system concepts underlying the implementation and how the project utilizes them.

---

### 1. Isolation Mechanisms

The runtime achieves isolation using Linux namespaces and filesystem techniques.

- **PID Namespace**  
  Provides each container with its own process ID space. Processes inside a container cannot see or interact with processes outside it.

- **UTS Namespace**  
  Allows containers to have independent hostnames, giving the illusion of separate systems.

- **Mount Namespace**  
  Creates an isolated filesystem view for each container, ensuring changes do not affect the host or other containers.

- **Filesystem Isolation (`chroot` / `pivot_root`)**  
  Restricts the container’s root directory to its own rootfs, preventing access to host files.

**Key Insight:**  
All containers share the same Linux kernel. This makes containers lightweight compared to virtual machines, as only OS-level isolation is provided rather than full hardware virtualization.

---

### 2. Supervisor and Process Lifecycle

A long-running supervisor process manages all containers.

- Containers are created using `clone()` with namespace flags  
- Each container is a child process of the supervisor  
- The supervisor tracks container metadata (ID, PID, state, exit status)

**Process Management:**
- `waitpid()` is used to reap child processes and prevent zombies  
- Signals are handled for lifecycle control:
  - `SIGINT`, `SIGTERM` → graceful shutdown  
  - `SIGKILL` → forced termination  

**Key Insight:**  
Without a supervisor, there would be no centralized control, leading to poor lifecycle management and potential zombie processes.

---

### 3. IPC, Threads, and Synchronization

The system uses two IPC mechanisms:

- **Pipes** → for transferring container stdout/stderr to the supervisor  
- **Socket/FIFO** → for CLI-to-supervisor communication  

#### Bounded Buffer Logging

A producer-consumer model is used:

- **Producers** read container output from pipes  
- **Consumers** write log data to files  

#### Synchronization Mechanisms

- **Mutex** → protects shared buffer and metadata  
- **Condition Variables** → coordinate producer and consumer execution  

#### Race Conditions Prevented

- Concurrent writes causing data corruption  
- Lost log messages  
- Deadlocks when buffer is full  

**Key Insight:**  
Proper synchronization ensures safe concurrent logging without data loss or system stalls.

---

### 4. Memory Management and Enforcement

- **RSS (Resident Set Size)** represents the actual physical memory used by a process  
- It does not include swapped-out memory or all shared memory  

#### Limit Policies

- **Soft Limit** → generates a warning when exceeded  
- **Hard Limit** → terminates the process  

#### Why Kernel-Space Enforcement?

- User-space monitoring is unreliable and can be bypassed  
- Kernel-space has direct access to process memory information  
- Enables immediate and secure enforcement  

**Implementation Insight:**  
The supervisor sends container PIDs via `ioctl`, and the kernel module tracks them using a linked list with proper locking.

---

### 5. Scheduling Behavior

Experiments were conducted using different workload types:

- **CPU-bound workload (`cpu_hog`)**  
- **I/O-bound workload (`io_pulse`)**

#### Observations

- CPU-bound processes consume more CPU time  
- I/O-bound processes are scheduled more frequently after I/O operations  
- Processes with lower `nice` values receive more CPU time  

#### Scheduler Behavior

Linux uses the **Completely Fair Scheduler (CFS)**:

- Distributes CPU time fairly among processes  
- Uses virtual runtime to balance execution  
- Improves responsiveness for interactive (I/O-bound) tasks  

**Key Insight:**  
The scheduler balances fairness, responsiveness, and throughput, which is observable through the workload experiments.

---

## Summary

This project demonstrates key operating system concepts:

- Process and filesystem isolation using namespaces  
- Centralized process management via a supervisor  
- Safe inter-process communication with synchronization  
- Kernel-level memory enforcement  
- Real-world scheduling behavior through controlled experiments  

These components together reflect how modern container systems operate at a fundamental level.

## Design Decisions and Tradeoffs

This section explains the key design choices made in each subsystem, the tradeoffs involved, and the justification for those decisions.

---

### 1. Namespace Isolation

**Design Choice:**  
Used Linux namespaces (`PID`, `UTS`, `mount`) along with `chroot` for filesystem isolation.

**Tradeoff:**  
While namespaces provide lightweight isolation, all containers still share the same kernel, which reduces isolation compared to virtual machines.

**Justification:**  
The goal of this project is to build a lightweight container runtime. Namespaces offer efficient isolation with minimal overhead, making them ideal for demonstrating OS-level virtualization without the complexity of full virtualization.

---

### 2. Supervisor Architecture

**Design Choice:**  
Implemented a long-running parent supervisor process to manage all containers.

**Tradeoff:**  
The supervisor becomes a single point of failure. If it crashes, management of all containers is lost.

**Justification:**  
A centralized supervisor simplifies lifecycle management, metadata tracking, and signal handling. It ensures proper cleanup (avoiding zombie processes) and provides a clear control interface for all containers.

---

### 3. IPC and Logging System

**Design Choice:**  
Used pipes for container output and a bounded-buffer producer-consumer model with threads for logging. Used a separate IPC mechanism (socket/FIFO) for CLI communication.

**Tradeoff:**  
The design introduces complexity due to multithreading and synchronization, increasing the risk of race conditions if not handled carefully.

**Justification:**  
Separating logging and control paths improves modularity and scalability. The bounded-buffer approach ensures no data loss and prevents blocking, which is essential for reliable concurrent logging.

---

### 4. Kernel Memory Monitor

**Design Choice:**  
Implemented memory monitoring and enforcement in a Linux Kernel Module using `ioctl` for communication.

**Tradeoff:**  
Kernel-space code is harder to debug and errors can affect system stability.

**Justification:**  
Memory enforcement must be reliable and cannot be bypassed. Kernel-space access allows precise tracking of process memory (RSS) and immediate enforcement of limits, which is not possible with user-space monitoring alone.

---

### 5. Scheduling Experiments

**Design Choice:**  
Used simple workload programs (`cpu_hog`, `io_pulse`) and varied scheduling parameters such as `nice` values.

**Tradeoff:**  
The experiments are simplified and may not capture all complexities of real-world workloads.

**Justification:**  
The goal is to demonstrate observable scheduling behavior clearly. Simple workloads make it easier to analyze and explain differences in CPU allocation, responsiveness, and fairness.

---

## Summary

Each design choice prioritizes simplicity, clarity, and alignment with operating system principles. While some tradeoffs exist (such as reduced isolation or added complexity), they are acceptable given the educational goals of the project and help demonstrate key OS concepts effectively.

## Scheduler Experiment Results

### Experiment Setup

Two CPU-bound containers were launched with different priorities using `nice` values:

```bash
sudo ./engine start alpha ./rootfs-alpha /cpu_hog --nice 0
sudo ./engine start beta ./rootfs-beta /cpu_hog --nice 10
top -d 1 -n 8
```

### Screenshot Evidence

<img width="1342" height="1042" alt="Screenshot 2026-04-19 150545" src="https://github.com/user-attachments/assets/521df05b-8aaa-493a-92ca-7afbbde2a7f2" />


### Raw Measurements

| Container | Workload | Nice Value | CPU Usage (Approx) | Behavior          |
|-----------|----------|------------|---------------------|-------------------|
| alpha     | cpu_hog  | 0          | ~80–90%             | Dominates CPU     |
| beta      | cpu_hog  | 10         | ~10–20%             | Reduced CPU share |

### Comparison

- Both containers run identical CPU-bound workloads  
- The only difference is the priority (`nice` value)  
- The scheduler assigns more CPU time to the higher-priority container (`nice = 0`)  

### Explanation of Results

Linux uses the **Completely Fair Scheduler (CFS)**:

- CPU time is distributed using **virtual runtime**  
- Lower nice values → higher priority  
- CPU-bound processes highlight scheduling differences clearly  

### Key Observations

- `nice = 0` container gets most CPU time  
- `nice = 10` container runs less frequently  
- CPU allocation is priority-based  

### Conclusion

- Linux scheduling is **fair but priority-aware**  
- Higher-priority processes receive more CPU time  
- Lower-priority processes are still scheduled, but less often  





