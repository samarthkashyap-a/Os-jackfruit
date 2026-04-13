#  Multi-Container Runtime
---

##  Project Overview  

This project focuses on building a lightweight container runtime in C on Linux.  
Instead of relying on tools like Docker, we implemented the core ideas ourselves to better understand how containers actually work internally.  

The system consists of a long-running supervisor process in user space and a kernel module for monitoring memory usage. Together, they allow us to run and manage multiple containers while maintaining isolation and enforcing resource limits.  

At a high level, the project is divided into two main components:  

---

##  Core Components  

### User-Space Runtime + Supervisor (`engine.c`)  

This is the main part of the system. It is responsible for:  

- Launching and managing multiple containers  
- Maintaining metadata for each container  
- Handling CLI commands  
- Capturing container output through a logging system  
- Managing container lifecycle and signals  

---

###  Kernel-Space Monitor (`monitor.c`)  

This is implemented as a Linux Kernel Module (LKM). It handles:  

- Tracking container processes  
- Monitoring memory usage (RSS)  
- Enforcing soft and hard memory limits  
- Communicating with user space via `ioctl`  

---

##  Environment Setup  

This project is designed to run in a proper Linux environment (VM recommended).  

### Requirements  
- Ubuntu 22.04 / 24.04  
- Secure Boot **disabled** (for kernel modules)  
- Not supported on WSL  

### Install dependencies  

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

---

##  Initial Setup  

### Run environment check  

```bash
cd boilerplate
chmod +x environment-check.sh
sudo ./environment-check.sh
```

### Prepare root filesystem  

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
```

### Create container-specific rootfs  

```bash
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

---

##  System Architecture  

The runtime uses a single binary (`engine`) in two modes:  

###  Supervisor Mode  
```bash
engine supervisor ./rootfs-base
```

- Runs continuously  
- Manages all containers  
- Handles logging and lifecycle  

###  CLI Mode  

Example:  

```bash
engine start alpha ./rootfs-alpha /bin/sh
```

- Sends request to supervisor  
- Receives response and exits  

---

##  Communication Design  

Two separate IPC mechanisms are used:  

###  Path A — Logging  
- Container → Supervisor  
- Uses pipes  
- Captures `stdout` and `stderr`  

###  Path B — Control  
- CLI → Supervisor  
- Uses IPC (socket / FIFO / shared memory)  

---

##  CLI Commands  

```bash
engine supervisor <base-rootfs>
engine start <id> <rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]
engine run   <id> <rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]
engine ps
engine logs <id>
engine stop <id>
```

---

##  Key Features  

###  Multi-Container Support  
- Multiple containers run simultaneously  
- Each container has isolated PID, UTS, and mount namespaces  
- Separate root filesystem per container  
- Proper child process handling (no zombies)  

---

###  Logging System  
- Producer-consumer model  
- Bounded buffer implementation  
- Captures both stdout and stderr  
- Ensures no data loss or deadlocks  

---

###  Memory Monitoring  
- Kernel module tracks processes  
- Soft limit → warning  
- Hard limit → process termination  
- Integrated with supervisor metadata  

---

###  Scheduling Experiments  
- Tested CPU-bound and I/O-bound workloads  
- Used different priorities (`nice`)  
- Observed behavior of Linux scheduler  

---

##  Resource Management  

Proper cleanup is handled across both user space and kernel space:  

- Child processes are reaped correctly  
- Logging threads terminate cleanly  
- File descriptors are closed  
- Kernel data structures are freed  
- No zombie processes remain  

---

##  Engineering Insights  

This project helped us understand several core OS concepts:  

###  Isolation  
Using namespaces and root filesystem separation, each container runs independently while still sharing the host kernel.  

###  Process Management  
The supervisor acts as a parent process, managing lifecycle, signals, and metadata of all containers.  

###  IPC & Synchronization  
We used multiple IPC mechanisms and synchronization primitives to safely manage shared resources.  

###  Memory Management  
RSS-based monitoring helped us enforce limits and understand memory usage behavior.  

###  Scheduling  
Running controlled experiments showed how Linux balances fairness, responsiveness, and throughput.  

---

##  Project Structure  

Required files in the repository:  

- `engine.c` — runtime and supervisor  
- `monitor.c` — kernel module  
- `monitor_ioctl.h` — shared interface  
- Test workloads  
- `Makefile`  
- `README.md`  

---

##  Example Run  

```bash
make
sudo insmod monitor.ko

sudo ./engine supervisor ./rootfs-base

cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta

sudo ./engine start alpha ./rootfs-alpha /bin/sh
sudo ./engine start beta ./rootfs-beta /bin/sh

sudo ./engine ps
sudo ./engine logs alpha

sudo ./engine stop alpha
sudo ./engine stop beta

dmesg | tail
sudo rmmod monitor
```

---

##  Demonstration  

The project includes demonstrations for:  

- Running multiple containers  
- Tracking metadata (`ps`)  
- Logging system behavior  
- CLI communication  
- Memory limit enforcement  
- Scheduling experiments  
- Clean shutdown  

---

##  Team Details  

**Team Size:** 2  

- **Bharat Shandilya**  
  SRN: PES2UG24CS906  

- **Samartha M S**  
  SRN: PES2UG24CS435  

---
