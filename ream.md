##  Build, Load, and Run Instructions  

###  Prerequisites  

Install required dependencies:  

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

---

###  Prepare Alpine Root Filesystem  

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

---

### Build the Project  

```bash
make
```

This builds:  
- `engine`  
- `monitor.ko`  
- `memory_hog`  
- `cpu_hog`  
- `io_pulse`  

---

###  Copy Workloads into Container Filesystems  

```bash
cp memory_hog cpu_hog io_pulse ./rootfs-alpha/
cp memory_hog cpu_hog io_pulse ./rootfs-beta/
```

---

###  Load Kernel Module  

```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
```

---

###  Start Supervisor (Terminal 1)  

```bash
sudo ./engine supervisor ./rootfs-base
```

---

###  Run CLI Commands (Terminal 2)  

```bash
sudo ./engine start alpha ./rootfs-alpha "/cpu_hog" --soft-mib 48 --hard-mib 80
sudo ./engine start beta  ./rootfs-beta  "/cpu_hog" --soft-mib 48 --hard-mib 80 --nice 10

sudo ./engine ps
sudo ./engine logs alpha

sudo ./engine stop alpha
sudo ./engine stop beta
```

---

###  Memory Limit Test  

```bash
sudo ./engine start alpha ./rootfs-alpha "/memory_hog" --soft-mib 10 --hard-mib 20
sudo ./engine start beta  ./rootfs-beta  "/memory_hog" --soft-mib 10 --hard-mib 20

sleep 8

sudo dmesg | grep container_monitor | tail -20
sudo ./engine ps
```

---

###  Scheduling Experiment  

```bash
sudo rm -f logs/alpha.log logs/beta.log

sudo ./engine start alpha ./rootfs-alpha "/cpu_hog" --nice 0  --soft-mib 48 --hard-mib 80
sudo ./engine start beta  ./rootfs-beta  "/cpu_hog" --nice 10 --soft-mib 48 --hard-mib 80

sleep 20

sudo ./engine stop alpha && sudo ./engine stop beta

echo "=== ALPHA ===" && sudo ./engine logs alpha | tail -5
echo "=== BETA  ===" && sudo ./engine logs beta  | tail -5
```

---

###  Teardown and Cleanup  

```bash
sudo ./engine stop alpha
sudo ./engine stop beta

# Stop supervisor (Ctrl + C in Terminal 1)

ps aux | grep defunct

sudo rmmod monitor
sudo dmesg | tail -3

make clean
```

---

