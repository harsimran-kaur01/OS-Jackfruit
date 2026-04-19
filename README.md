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
### Multi-container supervision
```
sudo ./engine start alpha ./rootfs-alpha /cpu_hog --nice 0

sudo ./engine start beta ./rootfs-beta /cpu_hog --nice 10

sudo ./engine ps
```
<img width="2559" height="773" alt="Screenshot 2026-04-19 103211" src="https://github.com/user-attachments/assets/4580e338-d235-4c7f-b1e7-ade81f78cae3" />

