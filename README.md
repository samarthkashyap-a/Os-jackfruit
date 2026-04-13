# Os-jackfruit
A lightweight multi-container runtime system that simulates core container lifecycle operations such as creation, execution, and management of multiple containers. This project demonstrates fundamental concepts of containerization, process isolation, and orchestration using a simplified runtime environment.

# 🚀 Multi-Container Runtime System  

## 📌 Description  

A simplified multi-container runtime system inspired by modern container engines like Docker.  

This project implements core runtime functionalities such as:  
- Container creation  
- Execution  
- Management of multiple containers  

It demonstrates key concepts such as process isolation, resource handling, and basic orchestration in a modular architecture.  

---

## 🎯 Objective  

The goal of this project is to design a lightweight runtime capable of managing multiple containers simultaneously.  

It simulates fundamental containerization concepts without relying on full-scale container platforms.  

---

## ⚙️ Features  

- Create and manage multiple containers  
- Start and stop container instances  
- Simulated container isolation using system processes  
- Multi-container execution (similar to docker-compose)  
- Modular and extensible design  

---

## 🧱 System Architecture  

### 1. Container Manager  

- Handles lifecycle operations  
- Create container  
- Start container  
- Stop container  

### 2. Runtime Engine  

- Executes container processes  
- Uses system calls like `fork()` and `exec()`  

### 3. Orchestrator  

- Manages multiple containers simultaneously  
- Ensures coordinated execution  

### 4. Configuration Layer  

- Reads container configuration files (JSON/YAML)  

---

## 🐳 Multi-Container Setup  

```yaml
version: '3'

services:
  app1:
    build:
      context: .
      dockerfile: Dockerfiles/app1.Dockerfile

  app2:
    build:
      context: .
      dockerfile: Dockerfiles/app2.Dockerfile
```

---

## ▶️ How to Run  

```bash
git clone <your-repo-link>
cd multi-container-runtime
chmod +x scripts/run.sh
./scripts/run.sh
```

---

## 💻 Technologies Used  

- C Programming  
- Linux System Calls  
- Docker (for multi-container simulation)  
- Shell Scripting  

---

## 📊 Sample Output  

```bash
Starting Multi-Container Runtime...
Creating container: container1
Starting container: container1
Creating container: container2
Starting container: container2
```

---

## ⚠️ Note  

This is a **simplified educational implementation** of a container runtime.  
It is intended for learning purposes and does not replace real-world tools like Docker or containerd.  

---

## 🚀 Future Enhancements  

- Add networking between containers  
- Implement resource limits (CPU/Memory)  
- Add logging and monitoring  
- Build a CLI interface  

---

## 👥 Team Details  

**Team Size:** 2  

- **Bharat Shandilya**  
  SRN: PES2UG24CS906  

- **Samartha M S**  
  SRN: PES2UG24CS435  
