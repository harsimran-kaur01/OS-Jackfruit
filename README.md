## Multi-Container Runtime — OS-Jackfruit
```
Team Information
Name	            SRN
Harsimran Kaur   PES1UG24CS187
Gitanjali A	     PES1UG24CS170
```
## Build, Load, and Run Instructions
### Prerequisites
```
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```
### Build
```
cd boilerplate
make ci

# Prepare Root Filesystems
mkdir -p rootfs-base

wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz

tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
```
## Copy workload binaries
```
cp cpu_hog memory_hog io_pulse rootfs-alpha/
cp cpu_hog memory_hog io_pulse rootfs-beta/
```
## Load Kernel Module
```
sudo insmod monitor.ko

# verify device
ls -l /dev/container_monitor

# verify load
sudo dmesg | tail -3
```
## Start Supervisor (Terminal 1)
```
sudo ./engine supervisor ./rootfs-base
```
## Use CLI (Terminal 2)
```
sudo ./engine start alpha ./rootfs-alpha /cpu_hog --soft-mib 48 --hard-mib 80
sudo ./engine ps
sudo ./engine logs alpha
```
## Start containers
```
sudo ./engine start alpha ./rootfs-alpha /cpu_hog --soft-mib 48 --hard-mib 80

sudo ./engine start beta ./rootfs-beta /cpu_hog --soft-mib 64 --hard-mib 96
```
## Run container (foreground
```
sudo ./engine run alpha ./rootfs-alpha /memory_hog --soft-mib 20 --hard-mib 64
```
## List containers
```
sudo ./engine ps
```
## View logs
```
sudo ./engine logs alpha
```
## Stop container
```
sudo ./engine stop alpha
```
## Remove Kernel Module + Check Logs + Clean
```
sudo rmmod monitor

sudo dmesg | tail -5

make clean
```
## Demo Screenshots
### 1)Multi-container supervision
```
sudo ./engine start alpha ./rootfs-alpha /cpu_hog --nice 0

sudo ./engine start beta ./rootfs-beta /cpu_hog --nice 10

sudo ./engine ps
```
<img width="2559" height="773" alt="Screenshot 2026-04-19 103211" src="https://github.com/user-attachments/assets/4580e338-d235-4c7f-b1e7-ade81f78cae3" />

### 2)Metadata tracking 
```
sudo ./engine ps
```
<img width="1104" height="131" alt="Screenshot 2026-04-19 113049" src="https://github.com/user-attachments/assets/712f85e4-1cb1-446c-b72f-d7169ab952a2" />


### 3)Logging system 
```
sudo ./engine start alpha ./rootfs-alpha /cpu_hog sleep 3
sudo ./engine logs alpha
```
<img width="1518" height="755" alt="Screenshot 2026-04-19 112558" src="https://github.com/user-attachments/assets/f34d051a-f361-4eed-afdf-44c275e0aec0" />

### 4) CLI and IPC 
```
sudo ./engine start alpha ./rootfs-alpha /cpu_hog --soft-mib 48 --hard-mib 80 sudo ./engine stop alpha sudo ./engine ps
```
<img width="1434" height="228" alt="image" src="https://github.com/user-attachments/assets/3b01f1ab-1312-4078-ac2b-33002ebe7fce" />
###  5)Soft-limit warning
```
sudo ./engine run alpha ./rootfs-alpha /memory_hog --soft-mib 20 --hard-mib 64 sudo dmesg | grep "SOFT LIMIT"
```
<img width="1503" height="315" alt="image" src="https://github.com/user-attachments/assets/925fa626-0a5f-4289-8550-c0b230979405" />
### 6) Hard-limit enforcement
```
sudo ./engine run alpha ./rootfs-alpha /memory_hog --soft-mib 10 --hard-mib 20 sudo dmesg | grep container_monitor | tail -5
sudo ./engine p
```
<img width="1492" height="682" alt="Screenshot 2026-04-19 114124" src="https://github.com/user-attachments/assets/09b43604-e315-427f-972e-135d4a7f7547" />

### 7) Scheduling experiment 
```
sudo ./engine start alpha ./rootfs-alpha /cpu_hog --nice 0
sudo ./engine start beta ./rootfs-beta /cpu_hog --nice 10 top -d 1 -n 8
```
<img width="1323" height="1023" alt="Screenshot 2026-04-19 114540" src="https://github.com/user-attachments/assets/6fee5325-a18f-41da-9284-6aa6440e57b9" />

### 8) Clean teardown 
```
ps aux | grep defunct
sudo rmmod monitor
sudo dmesg | tail -3
```
<img width="1354" height="269" alt="image" src="https://github.com/user-attachments/assets/2b8b9020-3dfb-4364-8b74-046ee6fb91fb" />

## Design and Implementation Overview
```
The system is designed using Linux namespaces such as PID, UTS, and Mount to provide process, hostname, and filesystem isolation, while each container is further isolated using chroot to give it an independent root filesystem. A central supervisor manages all containers by handling their lifecycle operations such as start, stop, and cleanup of processes to prevent zombie states. Communication between the CLI and supervisor is implemented using UNIX domain sockets, while logging is handled through pipes and stored in separate log files for each container. Memory management is enforced through soft and hard limits, where the soft limit triggers warnings and the hard limit results in process termination, with monitoring performed via a kernel module. CPU scheduling is demonstrated using Linux nice values, where lower nice values receive higher CPU priority and higher nice values receive reduced CPU access. The design choices prioritize simplicity and clarity, using chroot for lightweight filesystem isolation, a single supervisor for centralized control, pipes for efficient logging, UNIX sockets for fast inter-process communication, and nice values to demonstrate basic scheduling behavior. In experiments, a container with nice value 0 received nearly 100% CPU usage, while a container with nice value 10 received significantly less CPU time (around 60%), showing that lower nice values result in higher CPU allocation.
```




