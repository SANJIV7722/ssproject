#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef __USE_MINGW_ANSI_STDIO
#define __USE_MINGW_ANSI_STDIO 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <windows.h>
  #include <io.h>
  #include <process.h>
  #include <direct.h>
  #define getpid _getpid
  #define mkdir(d, m) _mkdir(d)
  #define fsync(fd) _commit(fd)
#else
  #include <unistd.h>
  #include <sys/types.h>
  #include <sys/stat.h>
  #include <dirent.h>
#endif

#include "storage.h"
#include "models.h"

#define DIR_DATA       "data"
#define DIR_SESSIONS   "data/sessions"
#define FILE_USERS     "data/users.txt"
#define FILE_ACCOUNTS  "data/accounts.txt"
#define FILE_TXS       "data/transactions.txt"
#define FILE_LOANS     "data/loans.txt"
#define FILE_FEEDBACK  "data/feedback.txt"
#define FILE_REQUESTS  "data/transfer_requests.txt"
#define FILE_LOCK      "data/bank.lock"

static int g_lock_fd = -1;

/* Low-level file helper: read entire file into a heap-allocated buffer */
static char* read_entire_file_fd(int fd, size_t *out_size) {
    off_t total_size = lseek(fd, 0, SEEK_END);
    if (total_size < 0) total_size = 0;
    lseek(fd, 0, SEEK_SET);

    char *buf = (char*)malloc(total_size + 1);
    if (!buf) {
        if (out_size) *out_size = 0;
        return NULL;
    }

    size_t bytes_read = 0;
    while (bytes_read < (size_t)total_size) {
        ssize_t n = read(fd, buf + bytes_read, total_size - bytes_read);
        if (n <= 0) break;
        bytes_read += n;
    }
    buf[bytes_read] = '\0';
    if (out_size) *out_size = bytes_read;
    return buf;
}

static char* read_entire_file(const char *path, size_t *out_size) {
    int fd = open(path, O_RDONLY, 0644);
    if (fd < 0) {
        if (out_size) *out_size = 0;
        return NULL;
    }
    char *buf = read_entire_file_fd(fd, out_size);
    close(fd);
    return buf;
}

/* Safe atomic write via temporary file */
static int write_file_atomic(const char *target_path, const char *content, size_t len) {
    char temp_path[MAX_PATH_LEN];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp.%d_%ld", target_path, (int)getpid(), (long)time(NULL));

    int fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return -1;
    }

    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, content + written, len - written);
        if (n <= 0) {
            close(fd);
            unlink(temp_path);
            return -1;
        }
        written += n;
    }

    fsync(fd);
    close(fd);

#ifdef _WIN32
    /* On Windows, rename fails if destination already exists, so unlink first */
    unlink(target_path);
#endif
    if (rename(temp_path, target_path) != 0) {
        unlink(temp_path);
        return -1;
    }
    return 0;
}

/* POSIX File Locking implementations */
int storage_lock_read(void) {
    if (g_lock_fd < 0) {
        g_lock_fd = open(FILE_LOCK, O_RDWR | O_CREAT, 0644);
        if (g_lock_fd < 0) return -1;
    }
#ifdef _WIN32
    HANDLE h = (HANDLE)_get_osfhandle(g_lock_fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED ov = {0};
    /* Shared read lock on Windows: flags = 0 */
    if (!LockFileEx(h, 0, 0, 0xFFFFFFFF, 0xFFFFFFFF, &ov)) {
        return -1;
    }
    return 0;
#else
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_RDLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    return fcntl(g_lock_fd, F_SETLKW, &fl);
#endif
}

int storage_lock_write(void) {
    if (g_lock_fd < 0) {
        g_lock_fd = open(FILE_LOCK, O_RDWR | O_CREAT, 0644);
        if (g_lock_fd < 0) return -1;
    }
#ifdef _WIN32
    HANDLE h = (HANDLE)_get_osfhandle(g_lock_fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED ov = {0};
    /* Exclusive write lock on Windows: flags = LOCKFILE_EXCLUSIVE_LOCK */
    if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK, 0, 0xFFFFFFFF, 0xFFFFFFFF, &ov)) {
        return -1;
    }
    return 0;
#else
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    return fcntl(g_lock_fd, F_SETLKW, &fl);
#endif
}

int storage_unlock(void) {
    if (g_lock_fd < 0) return 0;
#ifdef _WIN32
    HANDLE h = (HANDLE)_get_osfhandle(g_lock_fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED ov = {0};
    if (!UnlockFileEx(h, 0, 0xFFFFFFFF, 0xFFFFFFFF, &ov)) {
        return -1;
    }
    return 0;
#else
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    return fcntl(g_lock_fd, F_SETLK, &fl);
#endif
}

/* Session Management using session files */
int session_acquire(int user_id) {
    char session_path[MAX_PATH_LEN];
    snprintf(session_path, sizeof(session_path), "%s/%d.session", DIR_SESSIONS, user_id);

    /* O_CREAT | O_EXCL ensures atomic creation. If file exists, session is active! */
    int fd = open(session_path, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        return 0; /* Already logged in */
    }

    char content[64];
    int len = snprintf(content, sizeof(content), "pid=%d\ntime=%ld\n", (int)getpid(), (long)time(NULL));
    write(fd, content, len);
    close(fd);
    return 1; /* Successfully acquired session */
}

int session_release(int user_id) {
    char session_path[MAX_PATH_LEN];
    snprintf(session_path, sizeof(session_path), "%s/%d.session", DIR_SESSIONS, user_id);
    return unlink(session_path);
}

void session_cleanup_all(void) {
#ifdef _WIN32
    WIN32_FIND_DATA fd;
    HANDLE hFind = FindFirstFile("data\\sessions\\*.session", &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            char path[MAX_PATH_LEN];
            snprintf(path, sizeof(path), "data/sessions/%s", fd.cFileName);
            unlink(path);
        } while (FindNextFile(hFind, &fd));
        FindClose(hFind);
    }
#else
    DIR *d = opendir(DIR_SESSIONS);
    if (d) {
        struct dirent *dir;
        while ((dir = readdir(d)) != NULL) {
            if (strstr(dir->d_name, ".session")) {
                char path[MAX_PATH_LEN];
                snprintf(path, sizeof(path), "%s/%s", DIR_SESSIONS, dir->d_name);
                unlink(path);
            }
        }
        closedir(d);
    }
#endif
}

/* Storage Initialization */
int storage_init(void) {
    mkdir(DIR_DATA, 0755);
    mkdir(DIR_SESSIONS, 0755);

    /* Clean up stale sessions on server startup */
    session_cleanup_all();

    /* Initialize bank.lock */
    g_lock_fd = open(FILE_LOCK, O_RDWR | O_CREAT, 0644);
    if (g_lock_fd < 0) {
        perror("open bank.lock");
        return -1;
    }

    storage_lock_write();

    /* Check if users.txt exists. If not, seed initial data */
    int ufd = open(FILE_USERS, O_RDONLY, 0644);
    if (ufd < 0) {
        /* Create seed users:
         * 1|admin|admin123|ADMIN|1
         * 2|manager1|mgr123|MANAGER|1
         * 3|employee1|emp123|EMPLOYEE|1
         * 4|alice|alice123|CUSTOMER|1
         * 5|bob|bob123|CUSTOMER|1
         */
        const char *seed_users =
            "1|admin|admin123|ADMIN|1\n"
            "2|manager1|mgr123|MANAGER|1\n"
            "3|employee1|emp123|EMPLOYEE|1\n"
            "4|alice|alice123|CUSTOMER|1\n"
            "5|bob|bob123|CUSTOMER|1\n";
        write_file_atomic(FILE_USERS, seed_users, strlen(seed_users));

        /* Seed accounts:
         * 1001|4|50000000|1  (Alice has $500,000.00)
         * 1002|5|20000000|1  (Bob has $200,000.00)
         */
        const char *seed_accounts =
            "1001|4|50000000|1\n"
            "1002|5|20000000|1\n";
        write_file_atomic(FILE_ACCOUNTS, seed_accounts, strlen(seed_accounts));

        /* Empty files for transactions, loans, feedback, transfer_requests */
        int fd_tmp;
        fd_tmp = open(FILE_TXS, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_tmp >= 0) close(fd_tmp);
        fd_tmp = open(FILE_LOANS, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_tmp >= 0) close(fd_tmp);
        fd_tmp = open(FILE_FEEDBACK, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_tmp >= 0) close(fd_tmp);
        fd_tmp = open(FILE_REQUESTS, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_tmp >= 0) close(fd_tmp);
    } else {
        close(ufd);
    }

    storage_unlock();
    return 0;
}

/* Helper: safely split a pipe-delimited line into fields */
static int split_pipe(char *line, char **fields, int max_fields) {
    int count = 0;
    char *curr = line;
    while (curr && count < max_fields) {
        fields[count++] = curr;
        char *p = strchr(curr, '|');
        if (!p) break;
        *p = '\0';
        curr = p + 1;
    }
    return count;
}

/* User Operations */
int storage_get_all_users(User **out_users, int *out_count) {
    if (!out_users || !out_count) return -1;
    *out_users = NULL;
    *out_count = 0;

    size_t size = 0;
    char *buf = read_entire_file(FILE_USERS, &size);
    if (!buf) return 0;

    int capacity = 16;
    int count = 0;
    User *list = (User*)malloc(sizeof(User) * capacity);
    if (!list) { free(buf); return -1; }

    char *saveptr = NULL;
    char *line = strtok_r(buf, "\r\n", &saveptr);
    while (line) {
        if (strlen(line) > 0) {
            char *f[8];
            int n = split_pipe(line, f, 8);
            if (n >= 5) {
                User u;
                memset(&u, 0, sizeof(u));
                u.user_id = atoi(f[0]);
                strncpy(u.username, f[1], sizeof(u.username) - 1);
                strncpy(u.password_hash, f[2], sizeof(u.password_hash) - 1);
                u.role = string_to_role(f[3]);
                u.active = atoi(f[4]);

                if (count >= capacity) {
                    capacity *= 2;
                    list = (User*)realloc(list, sizeof(User) * capacity);
                }
                list[count++] = u;
            }
        }
        line = strtok_r(NULL, "\r\n", &saveptr);
    }

    free(buf);
    *out_users = list;
    *out_count = count;
    return 0;
}

int storage_find_user_by_username(const char *username, User *out_user) {
    if (!username || !out_user) return -1;
    User *users = NULL;
    int count = 0;
    if (storage_get_all_users(&users, &count) != 0) return -1;

    int found = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(users[i].username, username) == 0) {
            *out_user = users[i];
            found = 1;
            break;
        }
    }
    free(users);
    return found ? 0 : -1;
}

int storage_find_user_by_id(int user_id, User *out_user) {
    if (!out_user) return -1;
    User *users = NULL;
    int count = 0;
    if (storage_get_all_users(&users, &count) != 0) return -1;

    int found = 0;
    for (int i = 0; i < count; i++) {
        if (users[i].user_id == user_id) {
            *out_user = users[i];
            found = 1;
            break;
        }
    }
    free(users);
    return found ? 0 : -1;
}

int storage_add_user(const char *username, const char *password_hash, UserRole role, int *out_user_id) {
    if (!username || !password_hash) return -1;
    User *users = NULL;
    int count = 0;
    storage_get_all_users(&users, &count);

    int max_id = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(users[i].username, username) == 0) {
            free(users);
            return -2; /* Username already exists */
        }
        if (users[i].user_id > max_id) {
            max_id = users[i].user_id;
        }
    }
    int new_id = max_id + 1;

    size_t size = 0;
    char *buf = read_entire_file(FILE_USERS, &size);
    size_t new_size = size + 256;
    char *new_buf = (char*)malloc(new_size);
    if (size > 0 && buf) {
        memcpy(new_buf, buf, size);
    }
    int appended = snprintf(new_buf + size, new_size - size, "%d|%s|%s|%s|1\n",
                            new_id, username, password_hash, role_to_string(role));

    write_file_atomic(FILE_USERS, new_buf, size + appended);

    free(buf);
    free(new_buf);
    free(users);

    if (out_user_id) *out_user_id = new_id;
    return 0;
}

int storage_update_user(const User *user) {
    if (!user) return -1;
    User *users = NULL;
    int count = 0;
    if (storage_get_all_users(&users, &count) != 0) return -1;

    int found = 0;
    size_t alloc_size = count * 256 + 1024;
    char *buf = (char*)malloc(alloc_size);
    buf[0] = '\0';
    size_t offset = 0;

    for (int i = 0; i < count; i++) {
        User u = users[i];
        if (u.user_id == user->user_id) {
            u = *user;
            found = 1;
        }
        offset += snprintf(buf + offset, alloc_size - offset, "%d|%s|%s|%s|%d\n",
                           u.user_id, u.username, u.password_hash, role_to_string(u.role), u.active);
    }
    free(users);

    if (!found) {
        free(buf);
        return -1;
    }

    write_file_atomic(FILE_USERS, buf, offset);
    free(buf);
    return 0;
}

/* Account Operations */
int storage_get_all_accounts(Account **out_accounts, int *out_count) {
    if (!out_accounts || !out_count) return -1;
    *out_accounts = NULL;
    *out_count = 0;

    size_t size = 0;
    char *buf = read_entire_file(FILE_ACCOUNTS, &size);
    if (!buf) return 0;

    int capacity = 16;
    int count = 0;
    Account *list = (Account*)malloc(sizeof(Account) * capacity);
    if (!list) { free(buf); return -1; }

    char *saveptr = NULL;
    char *line = strtok_r(buf, "\r\n", &saveptr);
    while (line) {
        if (strlen(line) > 0) {
            char *f[8];
            int n = split_pipe(line, f, 8);
            if (n >= 4) {
                Account acc;
                memset(&acc, 0, sizeof(acc));
                acc.account_id = atoi(f[0]);
                acc.user_id = atoi(f[1]);
                acc.balance_cents = atoll(f[2]);
                acc.active = atoi(f[3]);

                if (count >= capacity) {
                    capacity *= 2;
                    list = (Account*)realloc(list, sizeof(Account) * capacity);
                }
                list[count++] = acc;
            }
        }
        line = strtok_r(NULL, "\r\n", &saveptr);
    }

    free(buf);
    *out_accounts = list;
    *out_count = count;
    return 0;
}

int storage_find_account_by_id(int account_id, Account *out_account) {
    if (!out_account) return -1;
    Account *accounts = NULL;
    int count = 0;
    if (storage_get_all_accounts(&accounts, &count) != 0) return -1;

    int found = 0;
    for (int i = 0; i < count; i++) {
        if (accounts[i].account_id == account_id) {
            *out_account = accounts[i];
            found = 1;
            break;
        }
    }
    free(accounts);
    return found ? 0 : -1;
}

int storage_find_account_by_user_id(int user_id, Account *out_account) {
    if (!out_account) return -1;
    Account *accounts = NULL;
    int count = 0;
    if (storage_get_all_accounts(&accounts, &count) != 0) return -1;

    int found = 0;
    for (int i = 0; i < count; i++) {
        if (accounts[i].user_id == user_id) {
            *out_account = accounts[i];
            found = 1;
            break;
        }
    }
    free(accounts);
    return found ? 0 : -1;
}

int storage_add_account(int user_id, long long initial_balance_cents, int *out_account_id) {
    Account *accounts = NULL;
    int count = 0;
    storage_get_all_accounts(&accounts, &count);

    int max_id = 1000;
    for (int i = 0; i < count; i++) {
        if (accounts[i].account_id > max_id) {
            max_id = accounts[i].account_id;
        }
    }
    int new_id = max_id + 1;

    size_t size = 0;
    char *buf = read_entire_file(FILE_ACCOUNTS, &size);
    size_t new_size = size + 128;
    char *new_buf = (char*)malloc(new_size);
    if (size > 0 && buf) {
        memcpy(new_buf, buf, size);
    }
    int appended = snprintf(new_buf + size, new_size - size, "%d|%d|%lld|1\n",
                            new_id, user_id, initial_balance_cents);

    write_file_atomic(FILE_ACCOUNTS, new_buf, size + appended);

    free(buf);
    free(new_buf);
    free(accounts);

    if (out_account_id) *out_account_id = new_id;
    return 0;
}

int storage_update_account(const Account *account) {
    if (!account) return -1;
    Account *accounts = NULL;
    int count = 0;
    if (storage_get_all_accounts(&accounts, &count) != 0) return -1;

    int found = 0;
    size_t alloc_size = count * 128 + 512;
    char *buf = (char*)malloc(alloc_size);
    buf[0] = '\0';
    size_t offset = 0;

    for (int i = 0; i < count; i++) {
        Account a = accounts[i];
        if (a.account_id == account->account_id) {
            a = *account;
            found = 1;
        }
        offset += snprintf(buf + offset, alloc_size - offset, "%d|%d|%lld|%d\n",
                           a.account_id, a.user_id, a.balance_cents, a.active);
    }
    free(accounts);

    if (!found) {
        free(buf);
        return -1;
    }

    write_file_atomic(FILE_ACCOUNTS, buf, offset);
    free(buf);
    return 0;
}

/* Transaction Operations */
int storage_get_all_transactions(Transaction **out_txs, int *out_count) {
    if (!out_txs || !out_count) return -1;
    *out_txs = NULL;
    *out_count = 0;

    size_t size = 0;
    char *buf = read_entire_file(FILE_TXS, &size);
    if (!buf) return 0;

    int capacity = 32;
    int count = 0;
    Transaction *list = (Transaction*)malloc(sizeof(Transaction) * capacity);
    if (!list) { free(buf); return -1; }

    char *saveptr = NULL;
    char *line = strtok_r(buf, "\r\n", &saveptr);
    while (line) {
        if (strlen(line) > 0) {
            char *f[10];
            int n = split_pipe(line, f, 10);
            if (n >= 7) {
                Transaction tx;
                memset(&tx, 0, sizeof(tx));
                tx.transaction_id = atoi(f[0]);
                tx.account_id = atoi(f[1]);
                strncpy(tx.type, f[2], sizeof(tx.type) - 1);
                tx.amount_cents = atoll(f[3]);
                tx.related_account_id = atoi(f[4]);
                strncpy(tx.details, f[5], sizeof(tx.details) - 1);
                tx.timestamp = (time_t)atoll(f[6]);

                if (count >= capacity) {
                    capacity *= 2;
                    list = (Transaction*)realloc(list, sizeof(Transaction) * capacity);
                }
                list[count++] = tx;
            }
        }
        line = strtok_r(NULL, "\r\n", &saveptr);
    }

    free(buf);
    *out_txs = list;
    *out_count = count;
    return 0;
}

int storage_add_transaction(int account_id, const char *type, long long amount_cents,
                            int related_account_id, const char *details, int *out_tx_id) {
    Transaction *txs = NULL;
    int count = 0;
    storage_get_all_transactions(&txs, &count);

    int max_id = 5000;
    for (int i = 0; i < count; i++) {
        if (txs[i].transaction_id > max_id) {
            max_id = txs[i].transaction_id;
        }
    }
    int new_id = max_id + 1;
    time_t now = time(NULL);

    size_t size = 0;
    char *buf = read_entire_file(FILE_TXS, &size);
    size_t new_size = size + 256;
    char *new_buf = (char*)malloc(new_size);
    if (size > 0 && buf) {
        memcpy(new_buf, buf, size);
    }
    int appended = snprintf(new_buf + size, new_size - size, "%d|%d|%s|%lld|%d|%s|%lld\n",
                            new_id, account_id, type, amount_cents, related_account_id,
                            (details && strlen(details) > 0) ? details : "-", (long long)now);

    write_file_atomic(FILE_TXS, new_buf, size + appended);

    free(buf);
    free(new_buf);
    free(txs);

    if (out_tx_id) *out_tx_id = new_id;
    return 0;
}

int storage_get_transactions_by_account(int account_id, Transaction **out_txs, int *out_count) {
    if (!out_txs || !out_count) return -1;
    Transaction *all = NULL;
    int total = 0;
    storage_get_all_transactions(&all, &total);

    int capacity = 16;
    int count = 0;
    Transaction *list = (Transaction*)malloc(sizeof(Transaction) * capacity);

    for (int i = 0; i < total; i++) {
        if (all[i].account_id == account_id) {
            if (count >= capacity) {
                capacity *= 2;
                list = (Transaction*)realloc(list, sizeof(Transaction) * capacity);
            }
            list[count++] = all[i];
        }
    }
    free(all);
    *out_txs = list;
    *out_count = count;
    return 0;
}

/* Loan Operations */
int storage_get_all_loans(Loan **out_loans, int *out_count) {
    if (!out_loans || !out_count) return -1;
    *out_loans = NULL;
    *out_count = 0;

    size_t size = 0;
    char *buf = read_entire_file(FILE_LOANS, &size);
    if (!buf) return 0;

    int capacity = 16;
    int count = 0;
    Loan *list = (Loan*)malloc(sizeof(Loan) * capacity);

    char *saveptr = NULL;
    char *line = strtok_r(buf, "\r\n", &saveptr);
    while (line) {
        if (strlen(line) > 0) {
            char *f[10];
            int n = split_pipe(line, f, 10);
            if (n >= 7) {
                Loan l;
                memset(&l, 0, sizeof(l));
                l.loan_id = atoi(f[0]);
                l.account_id = atoi(f[1]);
                l.amount_cents = atoll(f[2]);
                strncpy(l.status, f[3], sizeof(l.status) - 1);
                l.assigned_employee_id = atoi(f[4]);
                l.created_at = (time_t)atoll(f[5]);
                l.processed_at = (time_t)atoll(f[6]);

                if (count >= capacity) {
                    capacity *= 2;
                    list = (Loan*)realloc(list, sizeof(Loan) * capacity);
                }
                list[count++] = l;
            }
        }
        line = strtok_r(NULL, "\r\n", &saveptr);
    }

    free(buf);
    *out_loans = list;
    *out_count = count;
    return 0;
}

int storage_find_loan_by_id(int loan_id, Loan *out_loan) {
    if (!out_loan) return -1;
    Loan *loans = NULL;
    int count = 0;
    storage_get_all_loans(&loans, &count);

    int found = 0;
    for (int i = 0; i < count; i++) {
        if (loans[i].loan_id == loan_id) {
            *out_loan = loans[i];
            found = 1;
            break;
        }
    }
    free(loans);
    return found ? 0 : -1;
}

int storage_add_loan(int account_id, long long amount_cents, int *out_loan_id) {
    Loan *loans = NULL;
    int count = 0;
    storage_get_all_loans(&loans, &count);

    int max_id = 7000;
    for (int i = 0; i < count; i++) {
        if (loans[i].loan_id > max_id) max_id = loans[i].loan_id;
    }
    int new_id = max_id + 1;
    time_t now = time(NULL);

    size_t size = 0;
    char *buf = read_entire_file(FILE_LOANS, &size);
    size_t new_size = size + 256;
    char *new_buf = (char*)malloc(new_size);
    if (size > 0 && buf) memcpy(new_buf, buf, size);

    int appended = snprintf(new_buf + size, new_size - size, "%d|%d|%lld|PENDING|0|%lld|0\n",
                            new_id, account_id, amount_cents, (long long)now);

    write_file_atomic(FILE_LOANS, new_buf, size + appended);

    free(buf);
    free(new_buf);
    free(loans);

    if (out_loan_id) *out_loan_id = new_id;
    return 0;
}

int storage_update_loan(const Loan *loan) {
    if (!loan) return -1;
    Loan *loans = NULL;
    int count = 0;
    storage_get_all_loans(&loans, &count);

    int found = 0;
    size_t alloc_size = count * 256 + 512;
    char *buf = (char*)malloc(alloc_size);
    buf[0] = '\0';
    size_t offset = 0;

    for (int i = 0; i < count; i++) {
        Loan l = loans[i];
        if (l.loan_id == loan->loan_id) {
            l = *loan;
            found = 1;
        }
        offset += snprintf(buf + offset, alloc_size - offset, "%d|%d|%lld|%s|%d|%lld|%lld\n",
                           l.loan_id, l.account_id, l.amount_cents, l.status,
                           l.assigned_employee_id, (long long)l.created_at, (long long)l.processed_at);
    }
    free(loans);

    if (!found) { free(buf); return -1; }
    write_file_atomic(FILE_LOANS, buf, offset);
    free(buf);
    return 0;
}

/* Feedback Operations */
int storage_get_all_feedback(Feedback **out_feedbacks, int *out_count) {
    if (!out_feedbacks || !out_count) return -1;
    *out_feedbacks = NULL;
    *out_count = 0;

    size_t size = 0;
    char *buf = read_entire_file(FILE_FEEDBACK, &size);
    if (!buf) return 0;

    int capacity = 16;
    int count = 0;
    Feedback *list = (Feedback*)malloc(sizeof(Feedback) * capacity);

    char *saveptr = NULL;
    char *line = strtok_r(buf, "\r\n", &saveptr);
    while (line) {
        if (strlen(line) > 0) {
            char *f[6];
            int n = split_pipe(line, f, 6);
            if (n >= 4) {
                Feedback fb;
                memset(&fb, 0, sizeof(fb));
                fb.feedback_id = atoi(f[0]);
                fb.user_id = atoi(f[1]);
                strncpy(fb.message, f[2], sizeof(fb.message) - 1);
                fb.timestamp = (time_t)atoll(f[3]);

                if (count >= capacity) {
                    capacity *= 2;
                    list = (Feedback*)realloc(list, sizeof(Feedback) * capacity);
                }
                list[count++] = fb;
            }
        }
        line = strtok_r(NULL, "\r\n", &saveptr);
    }

    free(buf);
    *out_feedbacks = list;
    *out_count = count;
    return 0;
}

int storage_add_feedback(int user_id, const char *message, int *out_feedback_id) {
    Feedback *fbs = NULL;
    int count = 0;
    storage_get_all_feedback(&fbs, &count);

    int max_id = 9000;
    for (int i = 0; i < count; i++) {
        if (fbs[i].feedback_id > max_id) max_id = fbs[i].feedback_id;
    }
    int new_id = max_id + 1;
    time_t now = time(NULL);

    size_t size = 0;
    char *buf = read_entire_file(FILE_FEEDBACK, &size);
    size_t new_size = size + 512;
    char *new_buf = (char*)malloc(new_size);
    if (size > 0 && buf) memcpy(new_buf, buf, size);

    int appended = snprintf(new_buf + size, new_size - size, "%d|%d|%s|%lld\n",
                            new_id, user_id, message, (long long)now);

    write_file_atomic(FILE_FEEDBACK, new_buf, size + appended);

    free(buf);
    free(new_buf);
    free(fbs);

    if (out_feedback_id) *out_feedback_id = new_id;
    return 0;
}

/* Transfer Request Operations */
int storage_get_all_transfer_requests(TransferRequest **out_requests, int *out_count) {
    if (!out_requests || !out_count) return -1;
    *out_requests = NULL;
    *out_count = 0;

    size_t size = 0;
    char *buf = read_entire_file(FILE_REQUESTS, &size);
    if (!buf) return 0;

    int capacity = 16;
    int count = 0;
    TransferRequest *list = (TransferRequest*)malloc(sizeof(TransferRequest) * capacity);

    char *saveptr = NULL;
    char *line = strtok_r(buf, "\r\n", &saveptr);
    while (line) {
        if (strlen(line) > 0) {
            char *f[12];
            int n = split_pipe(line, f, 12);
            if (n >= 11) {
                TransferRequest tr;
                memset(&tr, 0, sizeof(tr));
                tr.request_id = atoi(f[0]);
                tr.sender_account_id = atoi(f[1]);
                tr.receiver_account_id = atoi(f[2]);
                tr.amount_cents = atoll(f[3]);
                tr.requested_by_user_id = atoi(f[4]);
                strncpy(tr.status, f[5], sizeof(tr.status) - 1);
                tr.created_at = (time_t)atoll(f[6]);
                tr.reviewed_by_user_id = atoi(f[7]);
                tr.reviewed_at = (time_t)atoll(f[8]);
                strncpy(tr.resolution_note, f[9], sizeof(tr.resolution_note) - 1);
                tr.transaction_id = atoi(f[10]);

                if (count >= capacity) {
                    capacity *= 2;
                    list = (TransferRequest*)realloc(list, sizeof(TransferRequest) * capacity);
                }
                list[count++] = tr;
            }
        }
        line = strtok_r(NULL, "\r\n", &saveptr);
    }

    free(buf);
    *out_requests = list;
    *out_count = count;
    return 0;
}

int storage_find_transfer_request_by_id(int request_id, TransferRequest *out_request) {
    if (!out_request) return -1;
    TransferRequest *requests = NULL;
    int count = 0;
    storage_get_all_transfer_requests(&requests, &count);

    int found = 0;
    for (int i = 0; i < count; i++) {
        if (requests[i].request_id == request_id) {
            *out_request = requests[i];
            found = 1;
            break;
        }
    }
    free(requests);
    return found ? 0 : -1;
}

int storage_add_transfer_request(int sender_account_id, int receiver_account_id,
                                 long long amount_cents, int requested_by_user_id,
                                 int *out_request_id) {
    TransferRequest *reqs = NULL;
    int count = 0;
    storage_get_all_transfer_requests(&reqs, &count);

    int max_id = 3000;
    for (int i = 0; i < count; i++) {
        if (reqs[i].request_id > max_id) max_id = reqs[i].request_id;
    }
    int new_id = max_id + 1;
    time_t now = time(NULL);

    size_t size = 0;
    char *buf = read_entire_file(FILE_REQUESTS, &size);
    size_t new_size = size + 512;
    char *new_buf = (char*)malloc(new_size);
    if (size > 0 && buf) memcpy(new_buf, buf, size);

    int appended = snprintf(new_buf + size, new_size - size,
                            "%d|%d|%d|%lld|%d|PENDING_APPROVAL|%lld|0|0|-|0\n",
                            new_id, sender_account_id, receiver_account_id,
                            amount_cents, requested_by_user_id, (long long)now);

    write_file_atomic(FILE_REQUESTS, new_buf, size + appended);

    free(buf);
    free(new_buf);
    free(reqs);

    if (out_request_id) *out_request_id = new_id;
    return 0;
}

int storage_update_transfer_request(const TransferRequest *request) {
    if (!request) return -1;
    TransferRequest *reqs = NULL;
    int count = 0;
    storage_get_all_transfer_requests(&reqs, &count);

    int found = 0;
    size_t alloc_size = count * 512 + 1024;
    char *buf = (char*)malloc(alloc_size);
    buf[0] = '\0';
    size_t offset = 0;

    for (int i = 0; i < count; i++) {
        TransferRequest r = reqs[i];
        if (r.request_id == request->request_id) {
            r = *request;
            found = 1;
        }
        offset += snprintf(buf + offset, alloc_size - offset,
                           "%d|%d|%d|%lld|%d|%s|%lld|%d|%lld|%s|%d\n",
                           r.request_id, r.sender_account_id, r.receiver_account_id,
                           r.amount_cents, r.requested_by_user_id, r.status,
                           (long long)r.created_at, r.reviewed_by_user_id, (long long)r.reviewed_at,
                           (r.resolution_note[0] != '\0') ? r.resolution_note : "-", r.transaction_id);
    }
    free(reqs);

    if (!found) { free(buf); return -1; }
    write_file_atomic(FILE_REQUESTS, buf, offset);
    free(buf);
    return 0;
}

/* Trusted Atomic Transfer Execution
 * Updates balances of sender and receiver, and adds both transaction records.
 * Caller MUST hold the write lock.
 */
int storage_execute_transfer(int sender_account_id, int receiver_account_id,
                             long long amount_cents, const char *details,
                             int *out_tx_id) {
    if (sender_account_id == receiver_account_id || amount_cents <= 0) {
        return -1;
    }

    Account sender, receiver;
    if (storage_find_account_by_id(sender_account_id, &sender) != 0) return -2;
    if (storage_find_account_by_id(receiver_account_id, &receiver) != 0) return -3;

    if (!sender.active) return -4;
    if (!receiver.active) return -5;

    if (sender.balance_cents < amount_cents) {
        return -6; /* Insufficient funds */
    }

    sender.balance_cents -= amount_cents;
    receiver.balance_cents += amount_cents;

    if (storage_update_account(&sender) != 0) return -7;
    if (storage_update_account(&receiver) != 0) {
        /* Rollback sender if receiver update fails */
        sender.balance_cents += amount_cents;
        storage_update_account(&sender);
        return -8;
    }

    int tx1 = 0, tx2 = 0;
    char d_sender[128], d_receiver[128];
    snprintf(d_sender, sizeof(d_sender), "Transfer to A/C %d: %s", receiver_account_id, details ? details : "");
    snprintf(d_receiver, sizeof(d_receiver), "Transfer from A/C %d: %s", sender_account_id, details ? details : "");

    storage_add_transaction(sender_account_id, "TRANSFER_OUT", amount_cents, receiver_account_id, d_sender, &tx1);
    storage_add_transaction(receiver_account_id, "TRANSFER_IN", amount_cents, sender_account_id, d_receiver, &tx2);

    if (out_tx_id) *out_tx_id = tx1;
    return 0;
}
