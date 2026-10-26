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
#include <stdint.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <io.h>
  #include <process.h>
  typedef int socklen_t;
  #define close_socket(s) closesocket(s)
#else
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <sys/wait.h>
  #include <signal.h>
  #define close_socket(s) close(s)
#endif

#include "models.h"
#include "protocol.h"
#include "storage.h"
#include "approval.h"

/* Socket communication helpers using standard read/write or recv/send */
static int send_msg(int sock, const char *msg) {
    size_t len = strlen(msg);
    size_t total = 0;
    while (total < len) {
#ifdef _WIN32
        int n = send(sock, msg + total, (int)(len - total), 0);
#else
        ssize_t n = write(sock, msg + total, len - total);
#endif
        if (n <= 0) return -1;
        total += n;
    }
#ifdef _WIN32
    send(sock, "\n", 1, 0);
#else
    write(sock, "\n", 1);
#endif
    return 0;
}

static int recv_line(int sock, char *buffer, size_t max_len) {
    size_t idx = 0;
    while (idx < max_len - 1) {
        char c;
#ifdef _WIN32
        int n = recv(sock, &c, 1, 0);
#else
        ssize_t n = read(sock, &c, 1);
#endif
        if (n <= 0) {
            if (idx == 0) return -1;
            break;
        }
        if (c == '\n') break;
        if (c != '\r') {
            buffer[idx++] = c;
        }
    }
    buffer[idx] = '\0';
    return (int)idx;
}



/* Dispatch client commands */
static void handle_client(int sock) {
    int auth_user_id = 0;
    UserRole auth_role = ROLE_UNKNOWN;
    char auth_username[64] = {0};

    char line[BUFFER_SIZE];
    char resp[BUFFER_SIZE * 4];

    while (recv_line(sock, line, sizeof(line)) > 0) {
        if (strlen(line) == 0) continue;

        char *cmd = strtok(line, "|");
        if (!cmd) continue;

        /* ---------------- LOGIN ---------------- */
        if (strcmp(cmd, CMD_LOGIN) == 0) {
            char *user = strtok(NULL, "|");
            char *pass = strtok(NULL, "|");
            if (!user || !pass) {
                send_msg(sock, "ERROR|MISSING_PARAMETERS|Usage: LOGIN|username|password");
                continue;
            }

            if (auth_user_id > 0) {
                send_msg(sock, "ERROR|ALREADY_AUTHENTICATED|You are already logged in on this session.");
                continue;
            }

            User u;
            if (storage_find_user_by_username(user, &u) != 0 || strcmp(u.password_hash, pass) != 0) {
                send_msg(sock, "ERROR|INVALID_CREDENTIALS|Incorrect username or password.");
                continue;
            }

            if (!u.active) {
                send_msg(sock, "ERROR|ACCOUNT_INACTIVE|Your account has been deactivated. Contact an administrator.");
                continue;
            }

            /* Single active session enforcement */
            if (!session_acquire(u.user_id)) {
                send_msg(sock, "ERROR|ALREADY_LOGGED_IN|This user is already logged in from another terminal.");
                continue;
            }

            auth_user_id = u.user_id;
            auth_role = u.role;
            strncpy(auth_username, u.username, sizeof(auth_username) - 1);

            snprintf(resp, sizeof(resp), "SUCCESS|LOGIN_OK|%d|%s|%s",
                     auth_user_id, role_to_string(auth_role), auth_username);
            send_msg(sock, resp);
            continue;
        }

        /* ---------------- LOGOUT ---------------- */
        if (strcmp(cmd, CMD_LOGOUT) == 0) {
            if (auth_user_id > 0) {
                session_release(auth_user_id);
                auth_user_id = 0;
                auth_role = ROLE_UNKNOWN;
                auth_username[0] = '\0';
            }
            send_msg(sock, "SUCCESS|LOGGED_OUT|Session ended successfully.");
            continue;
        }

        /* Require Authentication for all subsequent commands */
        if (auth_user_id == 0) {
            send_msg(sock, "ERROR|UNAUTHENTICATED|Please log in first.");
            continue;
        }

        /* ---------------- CHANGE PASSWORD ---------------- */
        if (strcmp(cmd, CMD_CHANGE_PASSWORD) == 0) {
            char *old_pass = strtok(NULL, "|");
            char *new_pass = strtok(NULL, "|");
            if (!old_pass || !new_pass) {
                send_msg(sock, "ERROR|MISSING_PARAMETERS|Usage: CHANGE_PASSWORD|old_pass|new_pass");
                continue;
            }

            storage_lock_write();
            User u;
            if (storage_find_user_by_id(auth_user_id, &u) != 0 || strcmp(u.password_hash, old_pass) != 0) {
                storage_unlock();
                send_msg(sock, "ERROR|INVALID_OLD_PASSWORD|Old password does not match.");
                continue;
            }

            strncpy(u.password_hash, new_pass, sizeof(u.password_hash) - 1);
            storage_update_user(&u);
            storage_unlock();

            send_msg(sock, "SUCCESS|PASSWORD_CHANGED|Password updated successfully.");
            continue;
        }

        /* ==============================================================
         * CUSTOMER COMMANDS
         * ============================================================== */

        /* ---------------- GET BALANCE ---------------- */
        if (strcmp(cmd, CMD_GET_BALANCE) == 0) {
            Account acc;
            if (storage_find_account_by_user_id(auth_user_id, &acc) != 0) {
                send_msg(sock, "ERROR|NO_ACCOUNT|No bank account found for your profile.");
                continue;
            }
            snprintf(resp, sizeof(resp), "SUCCESS|BALANCE|%d|%lld|%d",
                     acc.account_id, acc.balance_cents, acc.active);
            send_msg(sock, resp);
            continue;
        }

        /* ---------------- DEPOSIT ---------------- */
        if (strcmp(cmd, CMD_DEPOSIT) == 0) {
            char *amt_str = strtok(NULL, "|");
            if (!amt_str) {
                send_msg(sock, "ERROR|MISSING_PARAMETERS|Usage: DEPOSIT|amount_cents");
                continue;
            }
            long long amount = atoll(amt_str);
            if (amount <= 0) {
                send_msg(sock, "ERROR|INVALID_AMOUNT|Deposit amount must be positive.");
                continue;
            }

            storage_lock_write();
            Account acc;
            if (storage_find_account_by_user_id(auth_user_id, &acc) != 0 || !acc.active) {
                storage_unlock();
                send_msg(sock, "ERROR|ACCOUNT_INVALID|Active account not found.");
                continue;
            }

            acc.balance_cents += amount;
            storage_update_account(&acc);
            int tx_id = 0;
            storage_add_transaction(acc.account_id, "DEPOSIT", amount, -1, "Cash Deposit", &tx_id);
            storage_unlock();

            snprintf(resp, sizeof(resp), "SUCCESS|DEPOSIT_OK|%lld|%d", acc.balance_cents, tx_id);
            send_msg(sock, resp);
            continue;
        }

        /* ---------------- WITHDRAW ---------------- */
        if (strcmp(cmd, CMD_WITHDRAW) == 0) {
            char *amt_str = strtok(NULL, "|");
            if (!amt_str) {
                send_msg(sock, "ERROR|MISSING_PARAMETERS|Usage: WITHDRAW|amount_cents");
                continue;
            }
            long long amount = atoll(amt_str);
            if (amount <= 0) {
                send_msg(sock, "ERROR|INVALID_AMOUNT|Withdrawal amount must be positive.");
                continue;
            }

            storage_lock_write();
            Account acc;
            if (storage_find_account_by_user_id(auth_user_id, &acc) != 0 || !acc.active) {
                storage_unlock();
                send_msg(sock, "ERROR|ACCOUNT_INVALID|Active account not found.");
                continue;
            }

            if (acc.balance_cents < amount) {
                storage_unlock();
                send_msg(sock, "ERROR|INSUFFICIENT_FUNDS|Account balance is insufficient.");
                continue;
            }

            acc.balance_cents -= amount;
            storage_update_account(&acc);
            int tx_id = 0;
            storage_add_transaction(acc.account_id, "WITHDRAW", amount, -1, "Cash Withdrawal", &tx_id);
            storage_unlock();

            snprintf(resp, sizeof(resp), "SUCCESS|WITHDRAW_OK|%lld|%d", acc.balance_cents, tx_id);
            send_msg(sock, resp);
            continue;
        }

        /* ---------------- TRANSFER (Normal & Dual Auth) ---------------- */
        if (strcmp(cmd, CMD_TRANSFER) == 0) {
            char *recv_str = strtok(NULL, "|");
            char *amt_str = strtok(NULL, "|");
            if (!recv_str || !amt_str) {
                send_msg(sock, "ERROR|MISSING_PARAMETERS|Usage: TRANSFER|receiver_account_id|amount_cents");
                continue;
            }

            int receiver_id = atoi(recv_str);
            long long amount = atoll(amt_str);
            if (amount <= 0) {
                send_msg(sock, "ERROR|INVALID_AMOUNT|Transfer amount must be positive.");
                continue;
            }

            Account sender;
            if (storage_find_account_by_user_id(auth_user_id, &sender) != 0) {
                send_msg(sock, "ERROR|NO_ACCOUNT|No sender account linked to your user profile.");
                continue;
            }

            if (sender.account_id == receiver_id) {
                send_msg(sock, "ERROR|SAME_ACCOUNT|Cannot transfer to the same account.");
                continue;
            }

            int req_id = 0;
            int tx_id = 0;
            int res = submit_transfer_for_approval(auth_user_id, sender.account_id, receiver_id,
                                                   amount, &req_id, &tx_id);

            if (res == 1) {
                /* Completed immediately (below threshold) */
                snprintf(resp, sizeof(resp), "SUCCESS|TRANSFER_COMPLETED|%d|Direct transfer processed successfully.", tx_id);
                send_msg(sock, resp);
            } else if (res == 2) {
                /* High-value threshold met: pending approval */
                snprintf(resp, sizeof(resp), "SUCCESS|APPROVAL_PENDING|%d|Transfer amount meets/exceeds high-value threshold. Pending dual authorization by staff.", req_id);
                send_msg(sock, resp);
            } else if (res == APPROVAL_ERR_INSUFFICIENT_FUNDS) {
                send_msg(sock, "ERROR|INSUFFICIENT_FUNDS|Sender has insufficient balance.");
            } else if (res == APPROVAL_ERR_NOT_FOUND) {
                send_msg(sock, "ERROR|ACCOUNT_NOT_FOUND|Target receiver account not found.");
            } else if (res == APPROVAL_ERR_ACCOUNT_INACTIVE) {
                send_msg(sock, "ERROR|ACCOUNT_INACTIVE|One or both accounts are inactive.");
            } else {
                send_msg(sock, "ERROR|TRANSFER_FAILED|Failed to process transfer request.");
            }
            continue;
        }

        /* ---------------- VIEW TRANSFER STATUS ---------------- */
        if (strcmp(cmd, CMD_VIEW_TRANSFER_STATUS) == 0) {
            char *rid_str = strtok(NULL, "|");
            if (!rid_str) {
                send_msg(sock, "ERROR|MISSING_PARAMETERS|Usage: VIEW_TRANSFER_STATUS|request_id");
                continue;
            }
            int req_id = atoi(rid_str);

            TransferRequest req;
            int res = get_transfer_request_status(auth_user_id, auth_role, req_id, &req);
            if (res == APPROVAL_ERR_NOT_FOUND) {
                send_msg(sock, "ERROR|NOT_FOUND|Transfer request not found.");
            } else if (res == APPROVAL_ERR_NOT_AUTHORIZED) {
                send_msg(sock, "ERROR|NOT_AUTHORIZED|You are not authorized to view this request.");
            } else {
                snprintf(resp, sizeof(resp),
                         "SUCCESS|TRANSFER_STATUS|%d|%d|%d|%lld|%s|%lld|%d|%lld|%s|%d",
                         req.request_id, req.sender_account_id, req.receiver_account_id,
                         req.amount_cents, req.status, (long long)req.created_at,
                         req.reviewed_by_user_id, (long long)req.reviewed_at,
                         req.resolution_note, req.transaction_id);
                send_msg(sock, resp);
            }
            continue;
        }

        /* ---------------- CANCEL TRANSFER ---------------- */
        if (strcmp(cmd, CMD_CANCEL_TRANSFER) == 0) {
            char *rid_str = strtok(NULL, "|");
            if (!rid_str) {
                send_msg(sock, "ERROR|MISSING_PARAMETERS|Usage: CANCEL_TRANSFER|request_id");
                continue;
            }
            int req_id = atoi(rid_str);

            int res = cancel_transfer_request(auth_user_id, req_id);
            if (res == APPROVAL_OK) {
                snprintf(resp, sizeof(resp), "SUCCESS|TRANSFER_CANCELLED|%d|Request cancelled successfully.", req_id);
                send_msg(sock, resp);
            } else if (res == APPROVAL_ERR_NOT_AUTHORIZED) {
                send_msg(sock, "ERROR|NOT_AUTHORIZED|You can only cancel your own pending transfers.");
            } else if (res == APPROVAL_ERR_INVALID_STATE) {
                send_msg(sock, "ERROR|INVALID_STATE|Request is no longer pending approval.");
            } else {
                send_msg(sock, "ERROR|NOT_FOUND|Transfer request not found.");
            }
            continue;
        }

        /* ---------------- MY TRANSFERS ---------------- */
        if (strcmp(cmd, CMD_MY_TRANSFERS) == 0) {
            TransferRequest *reqs = NULL;
            int count = 0;
            list_user_transfer_requests(auth_user_id, &reqs, &count);

            snprintf(resp, sizeof(resp), "SUCCESS|MY_TRANSFERS|%d", count);
            send_msg(sock, resp);

            for (int i = 0; i < count; i++) {
                char item[512];
                snprintf(item, sizeof(item), "ITEM|%d|%d|%d|%lld|%s|%lld|%s|%d",
                         reqs[i].request_id, reqs[i].sender_account_id, reqs[i].receiver_account_id,
                         reqs[i].amount_cents, reqs[i].status, (long long)reqs[i].created_at,
                         reqs[i].resolution_note, reqs[i].transaction_id);
                send_msg(sock, item);
            }
            send_msg(sock, "END");
            free(reqs);
            continue;
        }

        /* ---------------- APPLY LOAN ---------------- */
        if (strcmp(cmd, CMD_APPLY_LOAN) == 0) {
            char *amt_str = strtok(NULL, "|");
            if (!amt_str) {
                send_msg(sock, "ERROR|MISSING_PARAMETERS|Usage: APPLY_LOAN|amount_cents");
                continue;
            }
            long long amount = atoll(amt_str);
            if (amount <= 0) {
                send_msg(sock, "ERROR|INVALID_AMOUNT|Loan amount must be positive.");
                continue;
            }

            Account acc;
            if (storage_find_account_by_user_id(auth_user_id, &acc) != 0 || !acc.active) {
                send_msg(sock, "ERROR|ACCOUNT_INVALID|Active account not found.");
                continue;
            }

            storage_lock_write();
            int loan_id = 0;
            storage_add_loan(acc.account_id, amount, &loan_id);
            storage_unlock();

            snprintf(resp, sizeof(resp), "SUCCESS|LOAN_APPLIED|%d|Loan application submitted for review.", loan_id);
            send_msg(sock, resp);
            continue;
        }

        /* ---------------- VIEW LOANS ---------------- */
        if (strcmp(cmd, CMD_VIEW_LOANS) == 0) {
            Account acc;
            if (storage_find_account_by_user_id(auth_user_id, &acc) != 0) {
                send_msg(sock, "ERROR|NO_ACCOUNT|No account found.");
                continue;
            }

            storage_lock_read();
            Loan *loans = NULL;
            int total = 0;
            storage_get_all_loans(&loans, &total);

            int match = 0;
            for (int i = 0; i < total; i++) {
                if (loans[i].account_id == acc.account_id) match++;
            }

            snprintf(resp, sizeof(resp), "SUCCESS|LOANS_LIST|%d", match);
            send_msg(sock, resp);

            for (int i = 0; i < total; i++) {
                if (loans[i].account_id == acc.account_id) {
                    char item[256];
                    snprintf(item, sizeof(item), "ITEM|%d|%lld|%s|%lld|%lld",
                             loans[i].loan_id, loans[i].amount_cents, loans[i].status,
                             (long long)loans[i].created_at, (long long)loans[i].processed_at);
                    send_msg(sock, item);
                }
            }
            send_msg(sock, "END");
            free(loans);
            storage_unlock();
            continue;
        }

        /* ---------------- SUBMIT FEEDBACK ---------------- */
        if (strcmp(cmd, CMD_SUBMIT_FEEDBACK) == 0) {
            char *msg = strtok(NULL, "|");
            if (!msg || strlen(msg) == 0) {
                send_msg(sock, "ERROR|MISSING_PARAMETERS|Usage: SUBMIT_FEEDBACK|message");
                continue;
            }

            storage_lock_write();
            int fb_id = 0;
            storage_add_feedback(auth_user_id, msg, &fb_id);
            storage_unlock();

            snprintf(resp, sizeof(resp), "SUCCESS|FEEDBACK_SUBMITTED|%d|Thank you for your feedback.", fb_id);
            send_msg(sock, resp);
            continue;
        }

        /* ---------------- VIEW TRANSACTIONS ---------------- */
        if (strcmp(cmd, CMD_VIEW_TRANSACTIONS) == 0) {
            Account acc;
            if (storage_find_account_by_user_id(auth_user_id, &acc) != 0) {
                send_msg(sock, "ERROR|NO_ACCOUNT|No account found.");
                continue;
            }

            storage_lock_read();
            Transaction *txs = NULL;
            int count = 0;
            storage_get_transactions_by_account(acc.account_id, &txs, &count);

            snprintf(resp, sizeof(resp), "SUCCESS|TX_LIST|%d", count);
            send_msg(sock, resp);

            for (int i = 0; i < count; i++) {
                char item[256];
                snprintf(item, sizeof(item), "ITEM|%d|%s|%lld|%d|%s|%lld",
                         txs[i].transaction_id, txs[i].type, txs[i].amount_cents,
                         txs[i].related_account_id, txs[i].details, (long long)txs[i].timestamp);
                send_msg(sock, item);
            }
            send_msg(sock, "END");
            free(txs);
            storage_unlock();
            continue;
        }

        /* ==============================================================
         * EMPLOYEE / MANAGER COMMANDS
         * ============================================================== */

        /* ---------------- VIEW PENDING TRANSFERS (Dual Auth) ---------------- */
        if (strcmp(cmd, CMD_VIEW_PENDING_TRANSFERS) == 0) {
            if (auth_role != ROLE_EMPLOYEE && auth_role != ROLE_MANAGER && auth_role != ROLE_ADMIN) {
                send_msg(sock, "ERROR|NOT_AUTHORIZED|Only bank staff may view pending high-value transfers.");
                continue;
            }

            TransferRequest *reqs = NULL;
            int count = 0;
            list_pending_transfer_requests(auth_user_id, auth_role, &reqs, &count);

            snprintf(resp, sizeof(resp), "SUCCESS|PENDING_TRANSFERS|%d", count);
            send_msg(sock, resp);

            for (int i = 0; i < count; i++) {
                char item[512];
                snprintf(item, sizeof(item), "ITEM|%d|%d|%d|%lld|%d|%lld|%s",
                         reqs[i].request_id, reqs[i].sender_account_id, reqs[i].receiver_account_id,
                         reqs[i].amount_cents, reqs[i].requested_by_user_id,
                         (long long)reqs[i].created_at, reqs[i].status);
                send_msg(sock, item);
            }
            send_msg(sock, "END");
            free(reqs);
            continue;
        }

        /* ---------------- APPROVE TRANSFER ---------------- */
        if (strcmp(cmd, CMD_APPROVE_TRANSFER) == 0) {
            char *rid_str = strtok(NULL, "|");
            char *note = strtok(NULL, "|");
            if (!rid_str) {
                send_msg(sock, "ERROR|MISSING_PARAMETERS|Usage: APPROVE_TRANSFER|request_id|resolution_note");
                continue;
            }
            int req_id = atoi(rid_str);

            int tx_id = 0;
            int res = approve_transfer_request(auth_user_id, auth_role, req_id, note ? note : "Approved", &tx_id);
            if (res == APPROVAL_OK) {
                snprintf(resp, sizeof(resp), "SUCCESS|TRANSFER_APPROVED|%d|%d|Transfer request approved and executed.", req_id, tx_id);
                send_msg(sock, resp);
            } else if (res == APPROVAL_ERR_SELF_APPROVAL) {
                send_msg(sock, "ERROR|SELF_APPROVAL_NOT_ALLOWED|Dual authorization violation: Requester cannot approve their own transfer.");
            } else if (res == APPROVAL_ERR_NOT_AUTHORIZED) {
                send_msg(sock, "ERROR|NOT_AUTHORIZED|You do not have permission to approve transfer requests.");
            } else if (res == APPROVAL_ERR_INVALID_STATE) {
                send_msg(sock, "ERROR|INVALID_STATE|Request is not pending approval.");
            } else if (res == APPROVAL_ERR_INSUFFICIENT_FUNDS) {
                send_msg(sock, "ERROR|INSUFFICIENT_FUNDS|Sender has insufficient balance. Request failed.");
            } else if (res == APPROVAL_ERR_ACCOUNT_INACTIVE) {
                send_msg(sock, "ERROR|ACCOUNT_INACTIVE|One of the accounts has been deactivated.");
            } else {
                send_msg(sock, "ERROR|NOT_FOUND|Transfer request not found.");
            }
            continue;
        }

        /* ---------------- REJECT TRANSFER ---------------- */
        if (strcmp(cmd, CMD_REJECT_TRANSFER) == 0) {
            char *rid_str = strtok(NULL, "|");
            char *note = strtok(NULL, "|");
            if (!rid_str) {
                send_msg(sock, "ERROR|MISSING_PARAMETERS|Usage: REJECT_TRANSFER|request_id|resolution_note");
                continue;
            }
            int req_id = atoi(rid_str);

            int res = reject_transfer_request(auth_user_id, auth_role, req_id, note ? note : "Rejected");
            if (res == APPROVAL_OK) {
                snprintf(resp, sizeof(resp), "SUCCESS|TRANSFER_REJECTED|%d|Transfer request rejected.", req_id);
                send_msg(sock, resp);
            } else if (res == APPROVAL_ERR_NOT_AUTHORIZED) {
                send_msg(sock, "ERROR|NOT_AUTHORIZED|You do not have permission to reject transfer requests.");
            } else if (res == APPROVAL_ERR_INVALID_STATE) {
                send_msg(sock, "ERROR|INVALID_STATE|Request is not pending approval.");
            } else {
                send_msg(sock, "ERROR|NOT_FOUND|Transfer request not found.");
            }
            continue;
        }

        /* ---------------- ADD ACCOUNT (For Customer) ---------------- */
        if (strcmp(cmd, CMD_ADD_ACCOUNT) == 0) {
            if (auth_role != ROLE_EMPLOYEE && auth_role != ROLE_MANAGER && auth_role != ROLE_ADMIN) {
                send_msg(sock, "ERROR|NOT_AUTHORIZED|Only bank staff may create accounts.");
                continue;
            }

            char *c_user = strtok(NULL, "|");
            char *dep_str = strtok(NULL, "|");
            if (!c_user || !dep_str) {
                send_msg(sock, "ERROR|MISSING_PARAMETERS|Usage: ADD_ACCOUNT|username|initial_deposit_cents");
                continue;
            }

            long long init_dep = atoll(dep_str);
            if (init_dep < 0) {
                send_msg(sock, "ERROR|INVALID_AMOUNT|Initial deposit cannot be negative.");
                continue;
            }

            storage_lock_write();
            User u;
            int u_id = 0;
            if (storage_find_user_by_username(c_user, &u) != 0) {
                /* Create user with default password */
                if (storage_add_user(c_user, "welcome123", ROLE_CUSTOMER, &u_id) != 0) {
                    storage_unlock();
                    send_msg(sock, "ERROR|USER_CREATE_FAILED|Failed to register customer user.");
                    continue;
                }
            } else {
                u_id = u.user_id;
            }

            int acc_id = 0;
            storage_add_account(u_id, init_dep, &acc_id);
            if (init_dep > 0) {
                int tx_id = 0;
                storage_add_transaction(acc_id, "DEPOSIT", init_dep, -1, "Initial Deposit", &tx_id);
            }
            storage_unlock();

            snprintf(resp, sizeof(resp), "SUCCESS|ACCOUNT_CREATED|%d|%d|Account created successfully with default pass: welcome123", acc_id, u_id);
            send_msg(sock, resp);
            continue;
        }

        /* ---------------- VIEW ALL ACCOUNTS ---------------- */
        if (strcmp(cmd, CMD_VIEW_ALL_ACCOUNTS) == 0) {
            if (auth_role != ROLE_EMPLOYEE && auth_role != ROLE_MANAGER && auth_role != ROLE_ADMIN) {
                send_msg(sock, "ERROR|NOT_AUTHORIZED|Permission denied.");
                continue;
            }

            storage_lock_read();
            Account *accs = NULL;
            int count = 0;
            storage_get_all_accounts(&accs, &count);

            snprintf(resp, sizeof(resp), "SUCCESS|ACCOUNT_LIST|%d", count);
            send_msg(sock, resp);

            for (int i = 0; i < count; i++) {
                User u;
                char uname[64] = "Unknown";
                if (storage_find_user_by_id(accs[i].user_id, &u) == 0) {
                    strncpy(uname, u.username, sizeof(uname) - 1);
                }
                char item[256];
                snprintf(item, sizeof(item), "ITEM|%d|%d|%s|%lld|%d",
                         accs[i].account_id, accs[i].user_id, uname,
                         accs[i].balance_cents, accs[i].active);
                send_msg(sock, item);
            }
            send_msg(sock, "END");
            free(accs);
            storage_unlock();
            continue;
        }

        /* ---------------- MODIFY CUSTOMER STATUS ---------------- */
        if (strcmp(cmd, CMD_MODIFY_CUSTOMER) == 0) {
            if (auth_role != ROLE_EMPLOYEE && auth_role != ROLE_MANAGER && auth_role != ROLE_ADMIN) {
                send_msg(sock, "ERROR|NOT_AUTHORIZED|Permission denied.");
                continue;
            }

            char *uid_str = strtok(NULL, "|");
            char *act_str = strtok(NULL, "|");
            if (!uid_str || !act_str) {
                send_msg(sock, "ERROR|MISSING_PARAMETERS|Usage: MODIFY_CUSTOMER|user_id|active_status");
                continue;
            }

            int uid = atoi(uid_str);
            int active = atoi(act_str);

            storage_lock_write();
            User u;
            if (storage_find_user_by_id(uid, &u) != 0) {
                storage_unlock();
                send_msg(sock, "ERROR|NOT_FOUND|User not found.");
                continue;
            }
            u.active = active;
            storage_update_user(&u);
            storage_unlock();

            send_msg(sock, "SUCCESS|CUSTOMER_MODIFIED|Customer status updated.");
            continue;
        }

        /* ---------------- VIEW PENDING LOANS ---------------- */
        if (strcmp(cmd, CMD_VIEW_PENDING_LOANS) == 0) {
            if (auth_role != ROLE_EMPLOYEE && auth_role != ROLE_MANAGER && auth_role != ROLE_ADMIN) {
                send_msg(sock, "ERROR|NOT_AUTHORIZED|Permission denied.");
                continue;
            }

            storage_lock_read();
            Loan *loans = NULL;
            int total = 0;
            storage_get_all_loans(&loans, &total);

            int count = 0;
            for (int i = 0; i < total; i++) {
                if (strcmp(loans[i].status, "PENDING") == 0) count++;
            }

            snprintf(resp, sizeof(resp), "SUCCESS|PENDING_LOANS|%d", count);
            send_msg(sock, resp);

            for (int i = 0; i < total; i++) {
                if (strcmp(loans[i].status, "PENDING") == 0) {
                    char item[256];
                    snprintf(item, sizeof(item), "ITEM|%d|%d|%lld|%lld",
                             loans[i].loan_id, loans[i].account_id,
                             loans[i].amount_cents, (long long)loans[i].created_at);
                    send_msg(sock, item);
                }
            }
            send_msg(sock, "END");
            free(loans);
            storage_unlock();
            continue;
        }

        /* ---------------- PROCESS LOAN ---------------- */
        if (strcmp(cmd, CMD_PROCESS_LOAN) == 0) {
            if (auth_role != ROLE_EMPLOYEE && auth_role != ROLE_MANAGER && auth_role != ROLE_ADMIN) {
                send_msg(sock, "ERROR|NOT_AUTHORIZED|Permission denied.");
                continue;
            }

            char *lid_str = strtok(NULL, "|");
            char *action = strtok(NULL, "|");
            if (!lid_str || !action) {
                send_msg(sock, "ERROR|MISSING_PARAMETERS|Usage: PROCESS_LOAN|loan_id|APPROVE/REJECT");
                continue;
            }

            int loan_id = atoi(lid_str);

            storage_lock_write();
            Loan l;
            if (storage_find_loan_by_id(loan_id, &l) != 0) {
                storage_unlock();
                send_msg(sock, "ERROR|NOT_FOUND|Loan application not found.");
                continue;
            }

            if (strcmp(l.status, "PENDING") != 0) {
                storage_unlock();
                send_msg(sock, "ERROR|INVALID_STATE|Loan is not pending.");
                continue;
            }

            l.assigned_employee_id = auth_user_id;
            l.processed_at = time(NULL);

            if (strcmp(action, "APPROVE") == 0) {
                strncpy(l.status, "APPROVED", sizeof(l.status) - 1);
                /* Disburse loan amount into customer's account */
                Account acc;
                if (storage_find_account_by_id(l.account_id, &acc) == 0) {
                    acc.balance_cents += l.amount_cents;
                    storage_update_account(&acc);
                    int tx_id = 0;
                    storage_add_transaction(acc.account_id, "LOAN_DISBURSEMENT", l.amount_cents, -1, "Loan Approved", &tx_id);
                }
            } else {
                strncpy(l.status, "REJECTED", sizeof(l.status) - 1);
            }

            storage_update_loan(&l);
            storage_unlock();

            snprintf(resp, sizeof(resp), "SUCCESS|LOAN_PROCESSED|%d|%s|Loan has been processed.", loan_id, l.status);
            send_msg(sock, resp);
            continue;
        }

        /* ---------------- VIEW CUSTOMER TX ---------------- */
        if (strcmp(cmd, CMD_VIEW_CUSTOMER_TX) == 0) {
            if (auth_role != ROLE_EMPLOYEE && auth_role != ROLE_MANAGER && auth_role != ROLE_ADMIN) {
                send_msg(sock, "ERROR|NOT_AUTHORIZED|Permission denied.");
                continue;
            }

            char *aid_str = strtok(NULL, "|");
            if (!aid_str) {
                send_msg(sock, "ERROR|MISSING_PARAMETERS|Usage: VIEW_CUSTOMER_TX|account_id");
                continue;
            }
            int acc_id = atoi(aid_str);

            storage_lock_read();
            Transaction *txs = NULL;
            int count = 0;
            storage_get_transactions_by_account(acc_id, &txs, &count);

            snprintf(resp, sizeof(resp), "SUCCESS|TX_LIST|%d", count);
            send_msg(sock, resp);

            for (int i = 0; i < count; i++) {
                char item[256];
                snprintf(item, sizeof(item), "ITEM|%d|%s|%lld|%d|%s|%lld",
                         txs[i].transaction_id, txs[i].type, txs[i].amount_cents,
                         txs[i].related_account_id, txs[i].details, (long long)txs[i].timestamp);
                send_msg(sock, item);
            }
            send_msg(sock, "END");
            free(txs);
            storage_unlock();
            continue;
        }

        /* ==============================================================
         * MANAGER EXCLUSIVE COMMANDS
         * ============================================================== */

        /* ---------------- VIEW FEEDBACK ---------------- */
        if (strcmp(cmd, CMD_VIEW_FEEDBACK) == 0) {
            if (auth_role != ROLE_MANAGER && auth_role != ROLE_ADMIN) {
                send_msg(sock, "ERROR|NOT_AUTHORIZED|Only Managers and Administrators can view feedback.");
                continue;
            }

            storage_lock_read();
            Feedback *fbs = NULL;
            int count = 0;
            storage_get_all_feedback(&fbs, &count);

            snprintf(resp, sizeof(resp), "SUCCESS|FEEDBACK_LIST|%d", count);
            send_msg(sock, resp);

            for (int i = 0; i < count; i++) {
                char item[512];
                snprintf(item, sizeof(item), "ITEM|%d|%d|%s|%lld",
                         fbs[i].feedback_id, fbs[i].user_id, fbs[i].message, (long long)fbs[i].timestamp);
                send_msg(sock, item);
            }
            send_msg(sock, "END");
            free(fbs);
            storage_unlock();
            continue;
        }

        /* ---------------- CHANGE ACCOUNT STATUS ---------------- */
        if (strcmp(cmd, CMD_CHANGE_ACCOUNT_STATUS) == 0) {
            if (auth_role != ROLE_MANAGER && auth_role != ROLE_ADMIN) {
                send_msg(sock, "ERROR|NOT_AUTHORIZED|Only Managers and Administrators can alter account active state.");
                continue;
            }

            char *aid_str = strtok(NULL, "|");
            char *act_str = strtok(NULL, "|");
            if (!aid_str || !act_str) {
                send_msg(sock, "ERROR|MISSING_PARAMETERS|Usage: CHANGE_ACCOUNT_STATUS|account_id|active_status");
                continue;
            }

            int acc_id = atoi(aid_str);
            int active = atoi(act_str);

            storage_lock_write();
            Account acc;
            if (storage_find_account_by_id(acc_id, &acc) != 0) {
                storage_unlock();
                send_msg(sock, "ERROR|NOT_FOUND|Account not found.");
                continue;
            }
            acc.active = active;
            storage_update_account(&acc);
            storage_unlock();

            send_msg(sock, "SUCCESS|ACCOUNT_STATUS_CHANGED|Account status updated successfully.");
            continue;
        }

        /* ==============================================================
         * ADMINISTRATOR COMMANDS
         * ============================================================== */

        /* ---------------- ADD STAFF ---------------- */
        if (strcmp(cmd, CMD_ADD_STAFF) == 0) {
            if (auth_role != ROLE_ADMIN) {
                send_msg(sock, "ERROR|NOT_AUTHORIZED|Only Administrators can add staff members.");
                continue;
            }

            char *uname = strtok(NULL, "|");
            char *pass = strtok(NULL, "|");
            char *role_str = strtok(NULL, "|");
            if (!uname || !pass || !role_str) {
                send_msg(sock, "ERROR|MISSING_PARAMETERS|Usage: ADD_STAFF|username|password|EMPLOYEE/MANAGER");
                continue;
            }

            UserRole nr = string_to_role(role_str);
            if (nr != ROLE_EMPLOYEE && nr != ROLE_MANAGER && nr != ROLE_ADMIN) {
                send_msg(sock, "ERROR|INVALID_ROLE|Role must be EMPLOYEE, MANAGER, or ADMIN.");
                continue;
            }

            storage_lock_write();
            int new_uid = 0;
            int r = storage_add_user(uname, pass, nr, &new_uid);
            storage_unlock();

            if (r == -2) {
                send_msg(sock, "ERROR|USERNAME_TAKEN|Username is already in use.");
            } else if (r == 0) {
                snprintf(resp, sizeof(resp), "SUCCESS|STAFF_ADDED|%d|%s added as %s.", new_uid, uname, role_str);
                send_msg(sock, resp);
            } else {
                send_msg(sock, "ERROR|SYSTEM_ERROR|Could not add staff user.");
            }
            continue;
        }

        /* ---------------- MODIFY USER ---------------- */
        if (strcmp(cmd, CMD_MODIFY_USER) == 0) {
            if (auth_role != ROLE_ADMIN) {
                send_msg(sock, "ERROR|NOT_AUTHORIZED|Only Administrators can modify user roles.");
                continue;
            }

            char *uid_str = strtok(NULL, "|");
            char *role_str = strtok(NULL, "|");
            char *act_str = strtok(NULL, "|");
            if (!uid_str || !role_str || !act_str) {
                send_msg(sock, "ERROR|MISSING_PARAMETERS|Usage: MODIFY_USER|user_id|role_str|active_status");
                continue;
            }

            int uid = atoi(uid_str);
            UserRole nr = string_to_role(role_str);
            int active = atoi(act_str);

            storage_lock_write();
            User u;
            if (storage_find_user_by_id(uid, &u) != 0) {
                storage_unlock();
                send_msg(sock, "ERROR|NOT_FOUND|User not found.");
                continue;
            }
            u.role = nr;
            u.active = active;
            storage_update_user(&u);
            storage_unlock();

            send_msg(sock, "SUCCESS|USER_MODIFIED|User attributes updated successfully.");
            continue;
        }

        /* ---------------- VIEW ALL USERS ---------------- */
        if (strcmp(cmd, CMD_VIEW_ALL_USERS) == 0) {
            if (auth_role != ROLE_ADMIN) {
                send_msg(sock, "ERROR|NOT_AUTHORIZED|Only Administrators can view all users.");
                continue;
            }

            storage_lock_read();
            User *users = NULL;
            int count = 0;
            storage_get_all_users(&users, &count);

            snprintf(resp, sizeof(resp), "SUCCESS|USER_LIST|%d", count);
            send_msg(sock, resp);

            for (int i = 0; i < count; i++) {
                char item[256];
                snprintf(item, sizeof(item), "ITEM|%d|%s|%s|%d",
                         users[i].user_id, users[i].username, role_to_string(users[i].role), users[i].active);
                send_msg(sock, item);
            }
            send_msg(sock, "END");
            free(users);
            storage_unlock();
            continue;
        }

        /* ---------------- ADMIN RESET PASSWORD ---------------- */
        if (strcmp(cmd, CMD_ADMIN_RESET_PASSWORD) == 0) {
            if (auth_role != ROLE_ADMIN) {
                send_msg(sock, "ERROR|NOT_AUTHORIZED|Only Administrators can reset other users' passwords.");
                continue;
            }

            char *uid_str = strtok(NULL, "|");
            char *new_pass = strtok(NULL, "|");
            if (!uid_str || !new_pass) {
                send_msg(sock, "ERROR|MISSING_PARAMETERS|Usage: ADMIN_RESET_PASSWORD|user_id|new_password");
                continue;
            }

            int uid = atoi(uid_str);
            storage_lock_write();
            User u;
            if (storage_find_user_by_id(uid, &u) != 0) {
                storage_unlock();
                send_msg(sock, "ERROR|NOT_FOUND|User not found.");
                continue;
            }

            strncpy(u.password_hash, new_pass, sizeof(u.password_hash) - 1);
            storage_update_user(&u);
            storage_unlock();

            snprintf(resp, sizeof(resp), "SUCCESS|PASSWORD_RESET|%d|Password reset successfully.", uid);
            send_msg(sock, resp);
            continue;
        }

        /* ---------------- VIEW SYSTEM STATS ---------------- */
        if (strcmp(cmd, CMD_VIEW_SYSTEM_STATS) == 0) {
            if (auth_role != ROLE_ADMIN) {
                send_msg(sock, "ERROR|NOT_AUTHORIZED|Permission denied.");
                continue;
            }

            storage_lock_read();
            User *users = NULL; int u_cnt = 0; storage_get_all_users(&users, &u_cnt); free(users);
            Account *accs = NULL; int a_cnt = 0; storage_get_all_accounts(&accs, &a_cnt); free(accs);
            Transaction *txs = NULL; int t_cnt = 0; storage_get_all_transactions(&txs, &t_cnt); free(txs);
            Loan *loans = NULL; int l_cnt = 0; storage_get_all_loans(&loans, &l_cnt); free(loans);
            TransferRequest *reqs = NULL; int r_cnt = 0; storage_get_all_transfer_requests(&reqs, &r_cnt);
            int pending_r = 0;
            for (int i = 0; i < r_cnt; i++) {
                if (strcmp(reqs[i].status, "PENDING_APPROVAL") == 0) pending_r++;
            }
            free(reqs);
            storage_unlock();

            snprintf(resp, sizeof(resp),
                     "SUCCESS|STATS|Users:%d|Accounts:%d|Transactions:%d|Loans:%d|PendingTransfers:%d",
                     u_cnt, a_cnt, t_cnt, l_cnt, pending_r);
            send_msg(sock, resp);
            continue;
        }

        /* ---------------- UNRECOGNIZED COMMAND ---------------- */
        send_msg(sock, "ERROR|UNKNOWN_COMMAND|Command unrecognized by banking server.");
    }

    /* Cleanup active session on connection drop */
    if (auth_user_id > 0) {
        session_release(auth_user_id);
    }
}

#ifdef _WIN32
static DWORD WINAPI win32_client_thread(LPVOID param) {
    int sock = (int)(intptr_t)param;
    handle_client(sock);
    close_socket(sock);
    return 0;
}
#endif

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int port = DEFAULT_PORT;
    if (argc > 1) {
        port = atoi(argv[1]);
        if (port <= 0) port = DEFAULT_PORT;
    }

    printf("=========================================================\n");
    printf("  BANKING MANAGEMENT SYSTEM - SERVER (with Dual Auth)\n");
    printf("=========================================================\n");

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", WSAGetLastError());
        return 1;
    }
#else
    /* Ignore SIGPIPE to avoid process termination on client disconnect */
    signal(SIGPIPE, SIG_IGN);
#endif

    if (storage_init() != 0) {
        fprintf(stderr, "Fatal: Failed to initialize storage layer.\n");
        return 1;
    }
    printf("[+] Storage layer initialized with POSIX file locking support.\n");
    printf("[+] High-Value Transfer Threshold: $%lld.00\n",
           HIGH_VALUE_TRANSFER_THRESHOLD_CENTS / 100);

    int server_fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
#ifdef _WIN32
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind");
        close_socket(server_fd);
        return 1;
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen");
        close_socket(server_fd);
        return 1;
    }

    printf("[+] Server listening on port %d on localhost...\n", port);
    printf("[+] Ready to accept client connections.\n\n");

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);
        int client_fd = (int)accept(server_fd, (struct sockaddr*)&client_addr, &addrlen);
        if (client_fd < 0) {
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            perror("accept");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        printf("[*] Connection accepted from %s:%d\n", client_ip, ntohs(client_addr.sin_port));

#ifdef _WIN32
        HANDLE hThread = CreateThread(NULL, 0, win32_client_thread, (LPVOID)(intptr_t)client_fd, 0, NULL);
        if (hThread) {
            CloseHandle(hThread);
        } else {
            close_socket(client_fd);
        }
#else
        pid_t pid = fork();
        if (pid == 0) {
            /* Child process handles client */
            close(server_fd);
            handle_client(client_fd);
            close(client_fd);
            exit(0);
        } else if (pid > 0) {
            /* Parent process */
            close(client_fd);
            /* Reap zombies */
            while (waitpid(-1, NULL, WNOHANG) > 0);
        } else {
            perror("fork");
            close(client_fd);
        }
#endif
    }

    close_socket(server_fd);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
