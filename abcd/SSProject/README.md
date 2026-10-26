# Banking Management System with Dual Authorization for High-Value Transfers

A robust, multi-tier Client-Server Banking Management System implemented from scratch in **C Language** using low-level POSIX and operating system calls. Built for concurrency, transactional consistency, role-based security, and dual authorization for high-value fund transfers.

---

## 1. System Architecture & Features

- **Networking & Concurrency**:
  - TCP client-server over localhost sockets (`socket`, `bind`, `listen`, `accept`, `connect`, `read`, `write`).
  - POSIX multi-client handling via `fork()` (and thread dispatch on Windows).
  - Clean signal handling and connection teardown.
- **Transactional Consistency & POSIX File Locking**:
  - Uses `data/bank.lock` with POSIX `fcntl()` advisory record locking (`F_SETLKW` with `F_RDLCK` and `F_WRLCK`).
  - Safe mutations via temporary file writes, `fsync()`, and atomic `rename()`.
- **Single Active Session Enforcement**:
  - Prevents concurrent logins with the same credentials across multiple client terminals via atomic session descriptors (`data/sessions/<user_id>.session` opened with `O_CREAT | O_EXCL`).
- **High-Value Transfer Dual Authorization**:
  - Transfers at or above threshold (`$100,000.00` = `10,000,000` cents) remain pending (`PENDING_APPROVAL`).
  - Requester balances and transaction logs remain **untouched** upon submission.
  - Mandatory second-party approval by authorized Employee or Manager.
  - **Strict Anti-Self-Approval**: Requesters are strictly prohibited from approving their own transfers, even if they hold an Employee, Manager, or Admin role.
  - Re-evaluates account active status and real-time available balances immediately before execution.
  - Rejection and customer-initiated cancellation ensure no balances are ever moved.
- **Full Banking Capabilities**:
  - **Customer**: Balance inquiry, cash deposit, cash withdrawal, direct transfers, high-value transfer submission/status/cancellation, loan application, loan status, feedback submission, full transaction history, password change.
  - **Employee**: Open customer accounts, modify customer details/status, inspect all accounts, inspect & process pending high-value transfers, inspect & process loan applications, customer transaction logs.
  - **Manager**: All employee privileges + freeze/activate accounts, inspect customer feedback submissions.
  - **Administrator**: Create employee/manager accounts, modify user roles, view all system users, administrative password reset, system statistics.

---

## 2. Project Structure

```text
SSProject/
├── models.h           # Data structures, roles, statuses, and currency thresholds
├── models.c           # Enum-to-string mapping and helper utilities
├── protocol.h         # Network protocol commands and response status codes
├── storage.h          # File storage, POSIX locking, and atomic commit prototypes
├── storage.c          # Low-level text-file persistence using system calls
├── approval.h         # High-value dual authorization module interface
├── approval.c         # Business validation, anti-self-approval, and state transitions
├── server.c           # Multi-client TCP server, request router, and session manager
├── client.c           # Role-tailored interactive CLI terminal client
├── Makefile           # POSIX & MinGW cross-platform build automation
├── build.bat          # Windows batch build script
├── test_e2e.py        # Automated end-to-end regression & authorization test suite
└── data/
    ├── users.txt             # user_id|username|password_hash|role|active
    ├── accounts.txt          # account_id|user_id|balance_cents|active
    ├── transactions.txt      # transaction_id|account_id|type|amount_cents|related_account_id|details|timestamp
    ├── loans.txt             # loan_id|account_id|amount_cents|status|assigned_employee_id|created_at|processed_at
    ├── feedback.txt          # feedback_id|user_id|message|timestamp
    ├── transfer_requests.txt # request_id|sender_acc|receiver_acc|amount_cents|req_uid|status|created_at|rev_uid|rev_at|note|tx_id
    ├── sessions/             # Atomic per-user active session tracking files
    └── bank.lock             # Dedicated lock file for POSIX fcntl concurrency control
```

---

## 3. Compilation & Building

### On Linux / POSIX:
```bash
make
```
Or directly using `gcc`:
```bash
gcc -std=gnu99 -Wall -Wextra -O2 -D_GNU_SOURCE -o server server.c storage.c approval.c models.c -pthread
gcc -std=gnu99 -Wall -Wextra -O2 -D_GNU_SOURCE -o client client.c models.c -pthread
```

### On Windows (MinGW):
```cmd
build.bat
```
Or via Makefile:
```cmd
make
```

---

## 4. Running the System

### 1. Launch the Banking Server:
```bash
./server 8080
```
On Windows:
```cmd
server.exe 8080
```

### 2. Launch Client Terminals:
Open one or more client terminals:
```bash
./client 127.0.0.1 8080
```
On Windows:
```cmd
client.exe 127.0.0.1 8080
```

### Default Pre-Seeded Users:
| Role | Username | Password | Notes |
| :--- | :--- | :--- | :--- |
| **Administrator** | `admin` | `admin123` | System admin, user management |
| **Manager** | `manager1` | `mgr123` | High-value transfer & loan approval |
| **Employee** | `employee1` | `emp123` | Account creation, approvals |
| **Customer** | `alice` | `alice123` | Account 1001 (Initial balance: $500,000.00) |
| **Customer** | `bob` | `bob123` | Account 1002 (Initial balance: $200,000.00) |

---

## 5. Automated Verification Test Suite

Run the comprehensive end-to-end verification script:
```bash
python test_e2e.py
```
This automatically verifies:
1. Single active session enforcement across terminals (`ALREADY_LOGGED_IN`).
2. Balance inquiries and minor currency representation (cents).
3. Immediate execution for transfers below threshold (`$500.00`).
4. Dual authorization queueing for transfers at/above `$100,000.00` with untouched balances.
5. Strict anti-self-approval enforcement (`SELF_APPROVAL_NOT_ALLOWED`).
6. Rejection of unauthorized customer approval attempts.
7. Authorized Manager approval, real-time balance re-validation, and atomic credit/debit commit.
8. Transfer rejection workflow (balances untouched).
9. Customer transfer cancellation (cannot be approved after cancellation).
10. Loan application and customer feedback workflows.
