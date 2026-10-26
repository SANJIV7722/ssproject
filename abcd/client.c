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

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <io.h>
  #define close_socket(s) closesocket(s)
#else
  #include <unistd.h>
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #define close_socket(s) close(s)
#endif

#include "models.h"
#include "protocol.h"

static int g_sock = -1;
static int g_user_id = 0;
static UserRole g_role = ROLE_UNKNOWN;
static char g_username[64] = {0};

/* Communication helpers */
static int send_cmd(const char *cmd) {
    size_t len = strlen(cmd);
    size_t total = 0;
    while (total < len) {
#ifdef _WIN32
        int n = send(g_sock, cmd + total, (int)(len - total), 0);
#else
        ssize_t n = write(g_sock, cmd + total, len - total);
#endif
        if (n <= 0) return -1;
        total += n;
    }
#ifdef _WIN32
    send(g_sock, "\n", 1, 0);
#else
    write(g_sock, "\n", 1);
#endif
    return 0;
}

static int recv_resp(char *buffer, size_t max_len) {
    size_t idx = 0;
    while (idx < max_len - 1) {
        char c;
#ifdef _WIN32
        int n = recv(g_sock, &c, 1, 0);
#else
        ssize_t n = read(g_sock, &c, 1);
#endif
        if (n <= 0) {
            if (idx == 0) return -1;
            break;
        }
        if (c == '\n') break;
        if (c != '\r') buffer[idx++] = c;
    }
    buffer[idx] = '\0';
    return (int)idx;
}

static void print_cents(long long cents) {
    long long dollars = cents / 100;
    long long rem = cents % 100;
    if (rem < 0) rem = -rem;
    printf("$%lld.%02lld", dollars, rem);
}

static void read_str(const char *prompt, char *buf, size_t sz) {
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(buf, (int)sz, stdin)) {
        buf[0] = '\0';
        return;
    }
    size_t l = strlen(buf);
    while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r')) {
        buf[--l] = '\0';
    }
}

static long long read_dollars_as_cents(const char *prompt) {
    char input[64];
    read_str(prompt, input, sizeof(input));
    double d = atof(input);
    if (d <= 0.0) return -1;
    return (long long)(d * 100.0 + 0.5);
}

/* Authentication */
static int do_login(void) {
    char username[64], password[64];
    printf("\n=== BANKING SYSTEM LOGIN ===\n");
    read_str("Username: ", username, sizeof(username));
    read_str("Password: ", password, sizeof(password));

    if (strlen(username) == 0 || strlen(password) == 0) {
        printf("[!] Username and password cannot be empty.\n");
        return 0;
    }

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "%s|%s|%s", CMD_LOGIN, username, password);
    if (send_cmd(cmd) != 0) {
        printf("[!] Network error sending credentials.\n");
        return 0;
    }

    char resp[BUFFER_SIZE];
    if (recv_resp(resp, sizeof(resp)) <= 0) {
        printf("[!] Server connection lost.\n");
        return 0;
    }

    char *status = strtok(resp, "|");
    if (status && strcmp(status, RESP_SUCCESS) == 0) {
        char *sub = strtok(NULL, "|");
        char *uid_str = strtok(NULL, "|");
        char *role_str = strtok(NULL, "|");
        char *uname = strtok(NULL, "|");

        (void)sub;
        g_user_id = uid_str ? atoi(uid_str) : 0;
        g_role = string_to_role(role_str);
        if (uname) strncpy(g_username, uname, sizeof(g_username) - 1);

        printf("\n[+] Login Successful! Welcome %s [%s] (User ID: %d)\n",
               g_username, role_to_string(g_role), g_user_id);
        return 1;
    } else {
        char *err_code = strtok(NULL, "|");
        char *err_msg = strtok(NULL, "|");
        printf("\n[-] Login Failed: %s\n", err_msg ? err_msg : (err_code ? err_code : "Unknown error"));
        return 0;
    }
}

static void do_logout(void) {
    send_cmd(CMD_LOGOUT);
    char resp[BUFFER_SIZE];
    recv_resp(resp, sizeof(resp));
    printf("[+] Logged out successfully.\n");
    g_user_id = 0;
    g_role = ROLE_UNKNOWN;
    g_username[0] = '\0';
}

static void do_change_password(void) {
    char old_p[64], new_p[64];
    read_str("Enter current password: ", old_p, sizeof(old_p));
    read_str("Enter new password: ", new_p, sizeof(new_p));

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "%s|%s|%s", CMD_CHANGE_PASSWORD, old_p, new_p);
    send_cmd(cmd);

    char resp[BUFFER_SIZE];
    recv_resp(resp, sizeof(resp));
    char *status = strtok(resp, "|");
    char *code = strtok(NULL, "|");
    char *msg = strtok(NULL, "|");
    printf("[%s] %s\n", strcmp(status, "SUCCESS") == 0 ? "+" : "!", msg ? msg : code);
}

/* ==============================================================
 * CUSTOMER OPERATIONS
 * ============================================================== */
static void customer_view_balance(void) {
    send_cmd(CMD_GET_BALANCE);
    char resp[BUFFER_SIZE];
    recv_resp(resp, sizeof(resp));

    char *status = strtok(resp, "|");
    if (status && strcmp(status, "SUCCESS") == 0) {
        strtok(NULL, "|");
        int acc_id = atoi(strtok(NULL, "|"));
        long long cents = atoll(strtok(NULL, "|"));
        int active = atoi(strtok(NULL, "|"));

        printf("\n--- ACCOUNT BALANCE ---");
        printf("\nAccount Number : %d", acc_id);
        printf("\nStatus         : %s", active ? "ACTIVE" : "INACTIVE/FROZEN");
        printf("\nCurrent Balance: ");
        print_cents(cents);
        printf("\n-----------------------\n");
    } else {
        char *code = strtok(NULL, "|");
        char *msg = strtok(NULL, "|");
        printf("[!] %s\n", msg ? msg : code);
    }
}

static void customer_deposit(void) {
    long long cents = read_dollars_as_cents("Enter deposit amount ($): ");
    if (cents <= 0) {
        printf("[!] Invalid amount.\n");
        return;
    }

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "%s|%lld", CMD_DEPOSIT, cents);
    send_cmd(cmd);

    char resp[BUFFER_SIZE];
    recv_resp(resp, sizeof(resp));
    char *status = strtok(resp, "|");
    if (status && strcmp(status, "SUCCESS") == 0) {
        strtok(NULL, "|");
        long long new_bal = atoll(strtok(NULL, "|"));
        int tx_id = atoi(strtok(NULL, "|"));
        printf("[+] Deposit complete. Tx ID: %d | New Balance: ", tx_id);
        print_cents(new_bal);
        printf("\n");
    } else {
        char *code = strtok(NULL, "|");
        char *msg = strtok(NULL, "|");
        printf("[!] %s\n", msg ? msg : code);
    }
}

static void customer_withdraw(void) {
    long long cents = read_dollars_as_cents("Enter withdrawal amount ($): ");
    if (cents <= 0) {
        printf("[!] Invalid amount.\n");
        return;
    }

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "%s|%lld", CMD_WITHDRAW, cents);
    send_cmd(cmd);

    char resp[BUFFER_SIZE];
    recv_resp(resp, sizeof(resp));
    char *status = strtok(resp, "|");
    if (status && strcmp(status, "SUCCESS") == 0) {
        strtok(NULL, "|");
        long long new_bal = atoll(strtok(NULL, "|"));
        int tx_id = atoi(strtok(NULL, "|"));
        printf("[+] Withdrawal complete. Tx ID: %d | New Balance: ", tx_id);
        print_cents(new_bal);
        printf("\n");
    } else {
        char *code = strtok(NULL, "|");
        char *msg = strtok(NULL, "|");
        printf("[!] %s\n", msg ? msg : code);
    }
}

static void customer_transfer(void) {
    char recv_buf[32];
    read_str("Enter beneficiary Account Number: ", recv_buf, sizeof(recv_buf));
    int target_acc = atoi(recv_buf);
    if (target_acc <= 0) {
        printf("[!] Invalid account number.\n");
        return;
    }

    long long cents = read_dollars_as_cents("Enter transfer amount ($): ");
    if (cents <= 0) {
        printf("[!] Invalid amount.\n");
        return;
    }

    printf("\n[*] Submitting transfer for ");
    print_cents(cents);
    printf(" to Account %d...\n", target_acc);

    if (cents >= HIGH_VALUE_TRANSFER_THRESHOLD_CENTS) {
        printf("[i] Notice: Transfer amount meets/exceeds high-value threshold ($100,000.00).\n");
        printf("    Dual authorization by a bank manager/employee will be required before funds move.\n");
    }

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "%s|%d|%lld", CMD_TRANSFER, target_acc, cents);
    send_cmd(cmd);

    char resp[BUFFER_SIZE];
    recv_resp(resp, sizeof(resp));

    char *status = strtok(resp, "|");
    char *code = strtok(NULL, "|");
    if (status && strcmp(status, "SUCCESS") == 0) {
        if (strcmp(code, "TRANSFER_COMPLETED") == 0) {
            int tx_id = atoi(strtok(NULL, "|"));
            char *msg = strtok(NULL, "|");
            printf("[+] Success! Direct Transfer Completed. Transaction ID: #%d\n    %s\n", tx_id, msg ? msg : "");
        } else if (strcmp(code, "APPROVAL_PENDING") == 0) {
            int req_id = atoi(strtok(NULL, "|"));
            char *msg = strtok(NULL, "|");
            printf("\n========================================================\n");
            printf(" [!] TRANSFER PENDING DUAL AUTHORIZATION\n");
            printf("     Request ID  : #%d\n", req_id);
            printf("     Status      : PENDING_APPROVAL\n");
            printf("     Details     : %s\n", msg ? msg : "");
            printf("     Note        : Account balance will NOT be debited\n");
            printf("                   until approved by bank staff.\n");
            printf("========================================================\n");
        }
    } else {
        char *msg = strtok(NULL, "|");
        printf("[!] Transfer Failed: %s\n", msg ? msg : code);
    }
}

static void customer_view_transfer_status(void) {
    char rid_buf[32];
    read_str("Enter Transfer Request ID: ", rid_buf, sizeof(rid_buf));
    int req_id = atoi(rid_buf);

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "%s|%d", CMD_VIEW_TRANSFER_STATUS, req_id);
    send_cmd(cmd);

    char resp[BUFFER_SIZE];
    recv_resp(resp, sizeof(resp));

    char *status = strtok(resp, "|");
    if (status && strcmp(status, "SUCCESS") == 0) {
        strtok(NULL, "|"); // TRANSFER_STATUS
        int r_id = atoi(strtok(NULL, "|"));
        int s_acc = atoi(strtok(NULL, "|"));
        int r_acc = atoi(strtok(NULL, "|"));
        long long amt = atoll(strtok(NULL, "|"));
        char *s_state = strtok(NULL, "|");
        long long c_at = atoll(strtok(NULL, "|"));
        int rev_by = atoi(strtok(NULL, "|"));
        long long rev_at = atoll(strtok(NULL, "|"));
        char *note = strtok(NULL, "|");
        int tx_id = atoi(strtok(NULL, "|"));

        printf("\n========================================================\n");
        printf("               TRANSFER REQUEST DETAILS                 \n");
        printf("========================================================\n");
        printf(" Request ID       : #%d\n", r_id);
        printf(" From Account     : %d\n", s_acc);
        printf(" To Account       : %d\n", r_acc);
        printf(" Amount           : "); print_cents(amt); printf("\n");
        printf(" Status           : %s\n", s_state);
        printf(" Submitted At     : %s", ctime((time_t*)&c_at));
        if (rev_by > 0) {
            printf(" Reviewed By Staff: User ID #%d\n", rev_by);
            printf(" Reviewed At      : %s", ctime((time_t*)&rev_at));
        }
        printf(" Resolution Note  : %s\n", note ? note : "-");
        if (tx_id > 0) {
            printf(" Executed Tx ID   : #%d\n", tx_id);
        }
        printf("========================================================\n");
    } else {
        char *code = strtok(NULL, "|");
        char *msg = strtok(NULL, "|");
        printf("[!] %s\n", msg ? msg : code);
    }
}

static void customer_my_transfers(void) {
    send_cmd(CMD_MY_TRANSFERS);
    char resp[BUFFER_SIZE];
    recv_resp(resp, sizeof(resp));

    char *status = strtok(resp, "|");
    if (!status || strcmp(status, "SUCCESS") != 0) {
        printf("[!] Failed to fetch transfer requests.\n");
        return;
    }

    strtok(NULL, "|");
    int count = atoi(strtok(NULL, "|"));
    printf("\n=== MY TRANSFER REQUESTS (%d found) ===\n", count);
    printf("%-8s | %-10s | %-10s | %-14s | %-16s | %-10s\n",
           "Req ID", "From A/C", "To A/C", "Amount", "Status", "Tx ID");
    printf("-----------------------------------------------------------------------\n");

    while (1) {
        char item[BUFFER_SIZE];
        if (recv_resp(item, sizeof(item)) <= 0) break;
        if (strcmp(item, "END") == 0) break;

        char *tag = strtok(item, "|");
        if (tag && strcmp(tag, "ITEM") == 0) {
            int rid = atoi(strtok(NULL, "|"));
            int s_acc = atoi(strtok(NULL, "|"));
            int r_acc = atoi(strtok(NULL, "|"));
            long long amt = atoll(strtok(NULL, "|"));
            char *state = strtok(NULL, "|");
            strtok(NULL, "|"); // created_at
            char *note = strtok(NULL, "|"); (void)note;
            int tx_id = atoi(strtok(NULL, "|"));

            char amt_str[32];
            snprintf(amt_str, sizeof(amt_str), "$%lld.%02lld", amt / 100, (amt % 100 < 0 ? -(amt % 100) : (amt % 100)));

            char tx_str[16];
            if (tx_id > 0) snprintf(tx_str, sizeof(tx_str), "#%d", tx_id);
            else snprintf(tx_str, sizeof(tx_str), "-");

            printf("#%-7d | %-10d | %-10d | %-14s | %-16s | %-10s\n",
                   rid, s_acc, r_acc, amt_str, state, tx_str);
        }
    }
    printf("-----------------------------------------------------------------------\n");
}

static void customer_cancel_transfer(void) {
    char rid_buf[32];
    read_str("Enter Transfer Request ID to cancel: ", rid_buf, sizeof(rid_buf));
    int req_id = atoi(rid_buf);

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "%s|%d", CMD_CANCEL_TRANSFER, req_id);
    send_cmd(cmd);

    char resp[BUFFER_SIZE];
    recv_resp(resp, sizeof(resp));

    char *status = strtok(resp, "|");
    char *code = strtok(NULL, "|");
    char *arg1 = strtok(NULL, "|");
    char *msg = strtok(NULL, "|");

    if (status && strcmp(status, "SUCCESS") == 0) {
        printf("[+] Request #%s: %s\n", arg1 ? arg1 : "", msg ? msg : "Cancelled.");
    } else {
        printf("[!] Failed to cancel: %s\n", arg1 ? arg1 : (code ? code : "Unknown error"));
    }
}

static void customer_apply_loan(void) {
    long long cents = read_dollars_as_cents("Enter loan amount requested ($): ");
    if (cents <= 0) {
        printf("[!] Invalid loan amount.\n");
        return;
    }

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "%s|%lld", CMD_APPLY_LOAN, cents);
    send_cmd(cmd);

    char resp[BUFFER_SIZE];
    recv_resp(resp, sizeof(resp));
    char *status = strtok(resp, "|");
    if (status && strcmp(status, "SUCCESS") == 0) {
        strtok(NULL, "|");
        int lid = atoi(strtok(NULL, "|"));
        char *msg = strtok(NULL, "|");
        printf("[+] Loan Application submitted successfully! Loan ID: #%d (%s)\n", lid, msg ? msg : "");
    } else {
        char *code = strtok(NULL, "|");
        char *msg = strtok(NULL, "|");
        printf("[!] %s\n", msg ? msg : code);
    }
}

static void customer_view_loans(void) {
    send_cmd(CMD_VIEW_LOANS);
    char resp[BUFFER_SIZE];
    recv_resp(resp, sizeof(resp));

    char *status = strtok(resp, "|");
    if (!status || strcmp(status, "SUCCESS") != 0) {
        printf("[!] Failed to retrieve loans.\n");
        return;
    }

    strtok(NULL, "|");
    int count = atoi(strtok(NULL, "|"));
    printf("\n=== MY LOANS (%d found) ===\n", count);
    printf("%-8s | %-14s | %-12s | %-24s\n", "Loan ID", "Amount", "Status", "Application Date");
    printf("-----------------------------------------------------------------\n");

    while (1) {
        char item[BUFFER_SIZE];
        if (recv_resp(item, sizeof(item)) <= 0) break;
        if (strcmp(item, "END") == 0) break;

        char *tag = strtok(item, "|");
        if (tag && strcmp(tag, "ITEM") == 0) {
            int lid = atoi(strtok(NULL, "|"));
            long long amt = atoll(strtok(NULL, "|"));
            char *st = strtok(NULL, "|");
            long long c_at = atoll(strtok(NULL, "|"));

            char amt_str[32];
            snprintf(amt_str, sizeof(amt_str), "$%lld.%02lld", amt / 100, amt % 100);
            char date_str[64];
            strncpy(date_str, ctime((time_t*)&c_at), sizeof(date_str) - 1);
            date_str[strlen(date_str) - 1] = '\0';

            printf("#%-7d | %-14s | %-12s | %-24s\n", lid, amt_str, st, date_str);
        }
    }
    printf("-----------------------------------------------------------------\n");
}

static void customer_submit_feedback(void) {
    char msg[256];
    read_str("Enter your feedback message for the branch manager: ", msg, sizeof(msg));
    if (strlen(msg) == 0) return;

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s|%s", CMD_SUBMIT_FEEDBACK, msg);
    send_cmd(cmd);

    char resp[BUFFER_SIZE];
    recv_resp(resp, sizeof(resp));
    char *status = strtok(resp, "|");
    if (status && strcmp(status, "SUCCESS") == 0) {
        printf("[+] Feedback submitted successfully. Thank you!\n");
    } else {
        printf("[!] Failed to submit feedback.\n");
    }
}

static void customer_view_transactions(void) {
    send_cmd(CMD_VIEW_TRANSACTIONS);
    char resp[BUFFER_SIZE];
    recv_resp(resp, sizeof(resp));

    char *status = strtok(resp, "|");
    if (!status || strcmp(status, "SUCCESS") != 0) {
        printf("[!] Failed to retrieve transactions.\n");
        return;
    }

    strtok(NULL, "|");
    int count = atoi(strtok(NULL, "|"));
    printf("\n=== TRANSACTION HISTORY (%d records) ===\n", count);
    printf("%-8s | %-16s | %-14s | %-12s | %-30s\n",
           "Tx ID", "Type", "Amount", "Related A/C", "Details");
    printf("------------------------------------------------------------------------------------\n");

    while (1) {
        char item[BUFFER_SIZE];
        if (recv_resp(item, sizeof(item)) <= 0) break;
        if (strcmp(item, "END") == 0) break;

        char *tag = strtok(item, "|");
        if (tag && strcmp(tag, "ITEM") == 0) {
            int tx_id = atoi(strtok(NULL, "|"));
            char *type = strtok(NULL, "|");
            long long amt = atoll(strtok(NULL, "|"));
            int rel = atoi(strtok(NULL, "|"));
            char *details = strtok(NULL, "|");

            char amt_str[32];
            snprintf(amt_str, sizeof(amt_str), "$%lld.%02lld", amt / 100, amt % 100);

            char rel_str[16];
            if (rel > 0) snprintf(rel_str, sizeof(rel_str), "%d", rel);
            else snprintf(rel_str, sizeof(rel_str), "-");

            printf("#%-7d | %-16s | %-14s | %-12s | %-30s\n",
                   tx_id, type, amt_str, rel_str, details ? details : "");
        }
    }
    printf("------------------------------------------------------------------------------------\n");
}

/* ==============================================================
 * EMPLOYEE / MANAGER OPERATIONS
 * ============================================================== */
static void staff_view_pending_transfers(void) {
    send_cmd(CMD_VIEW_PENDING_TRANSFERS);
    char resp[BUFFER_SIZE];
    recv_resp(resp, sizeof(resp));

    char *status = strtok(resp, "|");
    if (!status || strcmp(status, "SUCCESS") != 0) {
        printf("[!] Failed to retrieve pending transfers.\n");
        return;
    }

    strtok(NULL, "|");
    int count = atoi(strtok(NULL, "|"));
    printf("\n=== PENDING HIGH-VALUE TRANSFERS QUEUE (%d awaiting dual auth) ===\n", count);
    printf("%-8s | %-10s | %-10s | %-14s | %-14s | %-20s\n",
           "Req ID", "Sender A/C", "Beneficiary", "Amount", "Requester UID", "Submitted Time");
    printf("----------------------------------------------------------------------------------------\n");

    while (1) {
        char item[BUFFER_SIZE];
        if (recv_resp(item, sizeof(item)) <= 0) break;
        if (strcmp(item, "END") == 0) break;

        char *tag = strtok(item, "|");
        if (tag && strcmp(tag, "ITEM") == 0) {
            int rid = atoi(strtok(NULL, "|"));
            int s_acc = atoi(strtok(NULL, "|"));
            int r_acc = atoi(strtok(NULL, "|"));
            long long amt = atoll(strtok(NULL, "|"));
            int req_uid = atoi(strtok(NULL, "|"));
            long long c_at = atoll(strtok(NULL, "|"));

            char amt_str[32];
            snprintf(amt_str, sizeof(amt_str), "$%lld.%02lld", amt / 100, amt % 100);

            char time_str[32];
            struct tm *ti = localtime((time_t*)&c_at);
            if (ti) strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M", ti);
            else snprintf(time_str, sizeof(time_str), "-");

            printf("#%-7d | %-10d | %-10d | %-14s | UID #%-10d | %-20s\n",
                   rid, s_acc, r_acc, amt_str, req_uid, time_str);
        }
    }
    printf("----------------------------------------------------------------------------------------\n");
}

static void staff_approve_transfer(void) {
    char rid_buf[32], note[256];
    read_str("Enter Transfer Request ID to approve: ", rid_buf, sizeof(rid_buf));
    int req_id = atoi(rid_buf);
    read_str("Enter Approval Resolution Note: ", note, sizeof(note));

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s|%d|%s", CMD_APPROVE_TRANSFER, req_id, note);
    send_cmd(cmd);

    char resp[BUFFER_SIZE];
    recv_resp(resp, sizeof(resp));

    char *status = strtok(resp, "|");
    char *code = strtok(NULL, "|");
    if (status && strcmp(status, "SUCCESS") == 0) {
        int r_id = atoi(strtok(NULL, "|"));
        int tx_id = atoi(strtok(NULL, "|"));
        char *msg = strtok(NULL, "|");
        printf("\n[+] SUCCESS! Transfer Request #%d Approved and Executed!\n", r_id);
        printf("    Transaction ID : #%d\n", tx_id);
        printf("    Message        : %s\n", msg ? msg : "");
    } else {
        char *msg = strtok(NULL, "|");
        printf("\n[!] APPROVAL REJECTED: %s\n", msg ? msg : (code ? code : "Unknown error"));
        if (code && strcmp(code, "SELF_APPROVAL_NOT_ALLOWED") == 0) {
            printf("    [Security Policy] The requesting customer cannot approve their own high-value transfer.\n");
        }
    }
}

static void staff_reject_transfer(void) {
    char rid_buf[32], note[256];
    read_str("Enter Transfer Request ID to reject: ", rid_buf, sizeof(rid_buf));
    int req_id = atoi(rid_buf);
    read_str("Enter Rejection Reason: ", note, sizeof(note));

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s|%d|%s", CMD_REJECT_TRANSFER, req_id, note);
    send_cmd(cmd);

    char resp[BUFFER_SIZE];
    recv_resp(resp, sizeof(resp));

    char *status = strtok(resp, "|");
    char *code = strtok(NULL, "|");
    char *arg1 = strtok(NULL, "|");
    char *msg = strtok(NULL, "|");

    if (status && strcmp(status, "SUCCESS") == 0) {
        printf("[+] Transfer Request #%s rejected: %s\n", arg1 ? arg1 : "", msg ? msg : "");
    } else {
        printf("[!] Failed to reject: %s\n", arg1 ? arg1 : (code ? code : "Error"));
    }
}

static void staff_add_customer_account(void) {
    char uname[64];
    read_str("Enter customer username (will create user if new): ", uname, sizeof(uname));
    long long init_dep = read_dollars_as_cents("Enter initial opening deposit ($): ");
    if (init_dep < 0) init_dep = 0;

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "%s|%s|%lld", CMD_ADD_ACCOUNT, uname, init_dep);
    send_cmd(cmd);

    char resp[BUFFER_SIZE];
    recv_resp(resp, sizeof(resp));

    char *status = strtok(resp, "|");
    char *code = strtok(NULL, "|");
    if (status && strcmp(status, "SUCCESS") == 0) {
        int acc_id = atoi(strtok(NULL, "|"));
        int u_id = atoi(strtok(NULL, "|"));
        char *msg = strtok(NULL, "|");
        printf("[+] Account created! Account #: %d | User ID: %d\n    %s\n",
               acc_id, u_id, msg ? msg : "");
    } else {
        char *msg = strtok(NULL, "|");
        printf("[!] Failed: %s\n", msg ? msg : code);
    }
}

static void staff_view_all_accounts(void) {
    send_cmd(CMD_VIEW_ALL_ACCOUNTS);
    char resp[BUFFER_SIZE];
    recv_resp(resp, sizeof(resp));

    char *status = strtok(resp, "|");
    if (!status || strcmp(status, "SUCCESS") != 0) {
        printf("[!] Failed to retrieve accounts.\n");
        return;
    }

    strtok(NULL, "|");
    int count = atoi(strtok(NULL, "|"));
    printf("\n=== ALL BANK ACCOUNTS (%d registered) ===\n", count);
    printf("%-10s | %-10s | %-16s | %-14s | %-10s\n",
           "Account #", "User ID", "Username", "Balance", "Status");
    printf("----------------------------------------------------------------------\n");

    while (1) {
        char item[BUFFER_SIZE];
        if (recv_resp(item, sizeof(item)) <= 0) break;
        if (strcmp(item, "END") == 0) break;

        char *tag = strtok(item, "|");
        if (tag && strcmp(tag, "ITEM") == 0) {
            int acc_id = atoi(strtok(NULL, "|"));
            int uid = atoi(strtok(NULL, "|"));
            char *uname = strtok(NULL, "|");
            long long bal = atoll(strtok(NULL, "|"));
            int active = atoi(strtok(NULL, "|"));

            char bal_str[32];
            snprintf(bal_str, sizeof(bal_str), "$%lld.%02lld", bal / 100, bal % 100);

            printf("%-10d | %-10d | %-16s | %-14s | %-10s\n",
                   acc_id, uid, uname, bal_str, active ? "ACTIVE" : "INACTIVE");
        }
    }
    printf("----------------------------------------------------------------------\n");
}

static void staff_view_pending_loans(void) {
    send_cmd(CMD_VIEW_PENDING_LOANS);
    char resp[BUFFER_SIZE];
    recv_resp(resp, sizeof(resp));

    char *status = strtok(resp, "|");
    if (!status || strcmp(status, "SUCCESS") != 0) {
        printf("[!] Failed to retrieve loans.\n");
        return;
    }

    strtok(NULL, "|");
    int count = atoi(strtok(NULL, "|"));
    printf("\n=== PENDING LOAN APPLICATIONS (%d pending) ===\n", count);
    printf("%-8s | %-12s | %-14s | %-20s\n", "Loan ID", "Account #", "Amount", "Application Date");
    printf("--------------------------------------------------------------\n");

    while (1) {
        char item[BUFFER_SIZE];
        if (recv_resp(item, sizeof(item)) <= 0) break;
        if (strcmp(item, "END") == 0) break;

        char *tag = strtok(item, "|");
        if (tag && strcmp(tag, "ITEM") == 0) {
            int lid = atoi(strtok(NULL, "|"));
            int acc_id = atoi(strtok(NULL, "|"));
            long long amt = atoll(strtok(NULL, "|"));
            long long c_at = atoll(strtok(NULL, "|"));

            char amt_str[32];
            snprintf(amt_str, sizeof(amt_str), "$%lld.%02lld", amt / 100, amt % 100);
            char date_str[32];
            struct tm *ti = localtime((time_t*)&c_at);
            if (ti) strftime(date_str, sizeof(date_str), "%Y-%m-%d %H:%M", ti);
            else snprintf(date_str, sizeof(date_str), "-");

            printf("#%-7d | %-12d | %-14s | %-20s\n", lid, acc_id, amt_str, date_str);
        }
    }
    printf("--------------------------------------------------------------\n");
}

static void staff_process_loan(void) {
    char lid_buf[32], act_buf[16];
    read_str("Enter Loan ID to process: ", lid_buf, sizeof(lid_buf));
    int lid = atoi(lid_buf);
    read_str("Action (1 = APPROVE, 2 = REJECT): ", act_buf, sizeof(act_buf));

    const char *action = (atoi(act_buf) == 1) ? "APPROVE" : "REJECT";

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "%s|%d|%s", CMD_PROCESS_LOAN, lid, action);
    send_cmd(cmd);

    char resp[BUFFER_SIZE];
    recv_resp(resp, sizeof(resp));

    char *status = strtok(resp, "|");
    char *code = strtok(NULL, "|");
    char *arg1 = strtok(NULL, "|");
    char *arg2 = strtok(NULL, "|");
    char *msg = strtok(NULL, "|");

    if (status && strcmp(status, "SUCCESS") == 0) {
        printf("[+] Loan #%s marked %s: %s\n", arg1 ? arg1 : "", arg2 ? arg2 : "", msg ? msg : "");
    } else {
        printf("[!] Failed: %s\n", arg1 ? arg1 : (code ? code : "Error"));
    }
}

static void staff_view_customer_tx(void) {
    char aid_buf[32];
    read_str("Enter customer Account Number: ", aid_buf, sizeof(aid_buf));
    int acc_id = atoi(aid_buf);

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "%s|%d", CMD_VIEW_CUSTOMER_TX, acc_id);
    send_cmd(cmd);

    char resp[BUFFER_SIZE];
    recv_resp(resp, sizeof(resp));

    char *status = strtok(resp, "|");
    if (!status || strcmp(status, "SUCCESS") != 0) {
        printf("[!] Failed to retrieve transactions.\n");
        return;
    }

    strtok(NULL, "|");
    int count = atoi(strtok(NULL, "|"));
    printf("\n=== TRANSACTIONS FOR ACCOUNT %d (%d records) ===\n", acc_id, count);
    printf("%-8s | %-16s | %-14s | %-12s | %-30s\n",
           "Tx ID", "Type", "Amount", "Related A/C", "Details");
    printf("------------------------------------------------------------------------------------\n");

    while (1) {
        char item[BUFFER_SIZE];
        if (recv_resp(item, sizeof(item)) <= 0) break;
        if (strcmp(item, "END") == 0) break;

        char *tag = strtok(item, "|");
        if (tag && strcmp(tag, "ITEM") == 0) {
            int tx_id = atoi(strtok(NULL, "|"));
            char *type = strtok(NULL, "|");
            long long amt = atoll(strtok(NULL, "|"));
            int rel = atoi(strtok(NULL, "|"));
            char *details = strtok(NULL, "|");

            char amt_str[32];
            snprintf(amt_str, sizeof(amt_str), "$%lld.%02lld", amt / 100, amt % 100);

            printf("#%-7d | %-16s | %-14s | %-12d | %-30s\n",
                   tx_id, type, amt_str, rel, details ? details : "");
        }
    }
    printf("------------------------------------------------------------------------------------\n");
}

/* ==============================================================
 * MANAGER EXCLUSIVE OPERATIONS
 * ============================================================== */
static void manager_view_feedback(void) {
    send_cmd(CMD_VIEW_FEEDBACK);
    char resp[BUFFER_SIZE];
    recv_resp(resp, sizeof(resp));

    char *status = strtok(resp, "|");
    if (!status || strcmp(status, "SUCCESS") != 0) {
        printf("[!] Failed to retrieve feedback.\n");
        return;
    }

    strtok(NULL, "|");
    int count = atoi(strtok(NULL, "|"));
    printf("\n=== CUSTOMER FEEDBACK SUBMISSIONS (%d entries) ===\n", count);
    printf("%-8s | %-10s | %-40s | %-20s\n", "ID", "User ID", "Feedback Message", "Timestamp");
    printf("--------------------------------------------------------------------------------------\n");

    while (1) {
        char item[BUFFER_SIZE];
        if (recv_resp(item, sizeof(item)) <= 0) break;
        if (strcmp(item, "END") == 0) break;

        char *tag = strtok(item, "|");
        if (tag && strcmp(tag, "ITEM") == 0) {
            int f_id = atoi(strtok(NULL, "|"));
            int uid = atoi(strtok(NULL, "|"));
            char *msg = strtok(NULL, "|");
            long long ts = atoll(strtok(NULL, "|"));

            char date_str[32];
            struct tm *ti = localtime((time_t*)&ts);
            if (ti) strftime(date_str, sizeof(date_str), "%Y-%m-%d %H:%M", ti);
            else snprintf(date_str, sizeof(date_str), "-");

            printf("#%-7d | %-10d | %-40s | %-20s\n", f_id, uid, msg ? msg : "", date_str);
        }
    }
    printf("--------------------------------------------------------------------------------------\n");
}

static void manager_change_account_status(void) {
    char aid_buf[32], act_buf[16];
    read_str("Enter Account Number: ", aid_buf, sizeof(aid_buf));
    int acc_id = atoi(aid_buf);
    read_str("Set Status (1 = ACTIVE, 0 = INACTIVE/FROZEN): ", act_buf, sizeof(act_buf));
    int active = atoi(act_buf);

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "%s|%d|%d", CMD_CHANGE_ACCOUNT_STATUS, acc_id, active);
    send_cmd(cmd);

    char resp[BUFFER_SIZE];
    recv_resp(resp, sizeof(resp));
    char *status = strtok(resp, "|");
    char *code = strtok(NULL, "|");
    char *msg = strtok(NULL, "|");

    if (status && strcmp(status, "SUCCESS") == 0) {
        printf("[+] Account #%d status updated to %s.\n", acc_id, active ? "ACTIVE" : "INACTIVE");
    } else {
        printf("[!] Failed: %s\n", msg ? msg : code);
    }
}

/* ==============================================================
 * ADMINISTRATOR OPERATIONS
 * ============================================================== */
static void admin_add_staff(void) {
    char uname[64], pass[64], r_choice[16];
    read_str("Enter new staff username: ", uname, sizeof(uname));
    read_str("Enter temporary password: ", pass, sizeof(pass));
    read_str("Role (1 = EMPLOYEE, 2 = MANAGER): ", r_choice, sizeof(r_choice));

    const char *role_str = (atoi(r_choice) == 2) ? "MANAGER" : "EMPLOYEE";

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "%s|%s|%s|%s", CMD_ADD_STAFF, uname, pass, role_str);
    send_cmd(cmd);

    char resp[BUFFER_SIZE];
    recv_resp(resp, sizeof(resp));
    char *status = strtok(resp, "|");
    char *code = strtok(NULL, "|");
    char *arg1 = strtok(NULL, "|");
    char *msg = strtok(NULL, "|");

    if (status && strcmp(status, "SUCCESS") == 0) {
        printf("[+] %s\n", msg ? msg : "Staff added successfully.");
    } else {
        printf("[!] Failed: %s\n", arg1 ? arg1 : code);
    }
}

static void admin_modify_user(void) {
    char uid_buf[32], r_str[32], act_buf[16];
    read_str("Enter User ID to modify: ", uid_buf, sizeof(uid_buf));
    int uid = atoi(uid_buf);
    read_str("Enter Role (CUSTOMER, EMPLOYEE, MANAGER, ADMIN): ", r_str, sizeof(r_str));
    read_str("Set Active (1 = Active, 0 = Inactive): ", act_buf, sizeof(act_buf));
    int active = atoi(act_buf);

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "%s|%d|%s|%d", CMD_MODIFY_USER, uid, r_str, active);
    send_cmd(cmd);

    char resp[BUFFER_SIZE];
    recv_resp(resp, sizeof(resp));
    char *status = strtok(resp, "|");
    char *code = strtok(NULL, "|");
    char *msg = strtok(NULL, "|");

    if (status && strcmp(status, "SUCCESS") == 0) {
        printf("[+] User #%d updated successfully.\n", uid);
    } else {
        printf("[!] Failed: %s\n", msg ? msg : code);
    }
}

static void admin_view_all_users(void) {
    send_cmd(CMD_VIEW_ALL_USERS);
    char resp[BUFFER_SIZE];
    recv_resp(resp, sizeof(resp));

    char *status = strtok(resp, "|");
    if (!status || strcmp(status, "SUCCESS") != 0) {
        printf("[!] Failed to retrieve users.\n");
        return;
    }

    strtok(NULL, "|");
    int count = atoi(strtok(NULL, "|"));
    printf("\n=== ALL SYSTEM USERS (%d registered) ===\n", count);
    printf("%-8s | %-20s | %-16s | %-10s\n", "User ID", "Username", "Role", "Active");
    printf("------------------------------------------------------------\n");

    while (1) {
        char item[BUFFER_SIZE];
        if (recv_resp(item, sizeof(item)) <= 0) break;
        if (strcmp(item, "END") == 0) break;

        char *tag = strtok(item, "|");
        if (tag && strcmp(tag, "ITEM") == 0) {
            int uid = atoi(strtok(NULL, "|"));
            char *uname = strtok(NULL, "|");
            char *role = strtok(NULL, "|");
            int active = atoi(strtok(NULL, "|"));

            printf("%-8d | %-20s | %-16s | %-10s\n",
                   uid, uname, role, active ? "YES" : "NO (Blocked)");
        }
    }
    printf("------------------------------------------------------------\n");
}

static void admin_reset_password(void) {
    char uid_buf[32], new_pass[64];
    read_str("Enter User ID to reset password: ", uid_buf, sizeof(uid_buf));
    int uid = atoi(uid_buf);
    read_str("Enter new password: ", new_pass, sizeof(new_pass));

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "%s|%d|%s", CMD_ADMIN_RESET_PASSWORD, uid, new_pass);
    send_cmd(cmd);

    char resp[BUFFER_SIZE];
    recv_resp(resp, sizeof(resp));
    char *status = strtok(resp, "|");
    char *code = strtok(NULL, "|");
    char *arg1 = strtok(NULL, "|");
    char *msg = strtok(NULL, "|");

    if (status && strcmp(status, "SUCCESS") == 0) {
        printf("[+] Password for User #%s reset successfully: %s\n", arg1 ? arg1 : "", msg ? msg : "");
    } else {
        printf("[!] Failed: %s\n", arg1 ? arg1 : code);
    }
}

static void admin_view_stats(void) {
    send_cmd(CMD_VIEW_SYSTEM_STATS);
    char resp[BUFFER_SIZE];
    recv_resp(resp, sizeof(resp));

    char *status = strtok(resp, "|");
    if (status && strcmp(status, "SUCCESS") == 0) {
        strtok(NULL, "|"); // STATS
        char *u = strtok(NULL, "|");
        char *a = strtok(NULL, "|");
        char *t = strtok(NULL, "|");
        char *l = strtok(NULL, "|");
        char *p = strtok(NULL, "|");

        printf("\n=== SYSTEM OVERVIEW STATISTICS ===\n");
        printf(" %s\n %s\n %s\n %s\n %s\n",
               u ? u : "", a ? a : "", t ? t : "", l ? l : "", p ? p : "");
        printf("==================================\n");
    } else {
        printf("[!] Failed to get system stats.\n");
    }
}

/* ==============================================================
 * MAIN MENU LOOPS PER ROLE
 * ============================================================== */
static void run_customer_menu(void) {
    while (g_user_id > 0) {
        printf("\n============================================\n");
        printf("       CUSTOMER PORTAL - [%s]\n", g_username);
        printf("============================================\n");
        printf(" 1. View Account Balance\n");
        printf(" 2. Deposit Funds\n");
        printf(" 3. Withdraw Funds\n");
        printf(" 4. Transfer Money (Dual Auth for >= $100,000)\n");
        printf(" 5. View My Transfer Requests\n");
        printf(" 6. Inspect Specific Transfer Request Details\n");
        printf(" 7. Cancel Pending Transfer Request\n");
        printf(" 8. Apply for Loan\n");
        printf(" 9. View My Loans\n");
        printf(" 10. View Transaction History\n");
        printf(" 11. Submit Feedback to Branch Manager\n");
        printf(" 12. Change Password\n");
        printf(" 13. Logout\n");
        printf("============================================\n");

        char choice_str[16];
        read_str("Choose an option [1-13]: ", choice_str, sizeof(choice_str));
        int c = atoi(choice_str);

        switch (c) {
            case 1:  customer_view_balance(); break;
            case 2:  customer_deposit(); break;
            case 3:  customer_withdraw(); break;
            case 4:  customer_transfer(); break;
            case 5:  customer_my_transfers(); break;
            case 6:  customer_view_transfer_status(); break;
            case 7:  customer_cancel_transfer(); break;
            case 8:  customer_apply_loan(); break;
            case 9:  customer_view_loans(); break;
            case 10: customer_view_transactions(); break;
            case 11: customer_submit_feedback(); break;
            case 12: do_change_password(); break;
            case 13: do_logout(); return;
            default: printf("[!] Invalid selection.\n"); break;
        }
    }
}

static void run_employee_menu(void) {
    while (g_user_id > 0) {
        printf("\n=======================================================\n");
        printf("             EMPLOYEE PORTAL - [%s]\n", g_username);
        printf("=======================================================\n");
        printf(" 1. View Pending High-Value Transfers (Dual Auth Queue)\n");
        printf(" 2. Inspect Transfer Request Details\n");
        printf(" 3. Approve High-Value Transfer Request\n");
        printf(" 4. Reject High-Value Transfer Request\n");
        printf(" 5. Open New Customer Account\n");
        printf(" 6. View All Bank Accounts\n");
        printf(" 7. View Customer Transaction History\n");
        printf(" 8. View Pending Loan Applications\n");
        printf(" 9. Process Loan Application (Approve/Reject)\n");
        printf(" 10. Change Password\n");
        printf(" 11. Logout\n");
        printf("=======================================================\n");

        char choice_str[16];
        read_str("Choose an option [1-11]: ", choice_str, sizeof(choice_str));
        int c = atoi(choice_str);

        switch (c) {
            case 1:  staff_view_pending_transfers(); break;
            case 2:  customer_view_transfer_status(); break;
            case 3:  staff_approve_transfer(); break;
            case 4:  staff_reject_transfer(); break;
            case 5:  staff_add_customer_account(); break;
            case 6:  staff_view_all_accounts(); break;
            case 7:  staff_view_customer_tx(); break;
            case 8:  staff_view_pending_loans(); break;
            case 9:  staff_process_loan(); break;
            case 10: do_change_password(); break;
            case 11: do_logout(); return;
            default: printf("[!] Invalid selection.\n"); break;
        }
    }
}

static void run_manager_menu(void) {
    while (g_user_id > 0) {
        printf("\n=======================================================\n");
        printf("             MANAGER PORTAL - [%s]\n", g_username);
        printf("=======================================================\n");
        printf(" 1. View Pending High-Value Transfers (Dual Auth Queue)\n");
        printf(" 2. Inspect Transfer Request Details\n");
        printf(" 3. Approve High-Value Transfer Request\n");
        printf(" 4. Reject High-Value Transfer Request\n");
        printf(" 5. Open New Customer Account\n");
        printf(" 6. View All Bank Accounts\n");
        printf(" 7. Change Account Status (Activate/Freeze)\n");
        printf(" 8. View Customer Transaction History\n");
        printf(" 9. View Pending Loan Applications\n");
        printf(" 10. Process Loan Application (Approve/Reject)\n");
        printf(" 11. View Customer Feedback Submissions\n");
        printf(" 12. Change Password\n");
        printf(" 13. Logout\n");
        printf("=======================================================\n");

        char choice_str[16];
        read_str("Choose an option [1-13]: ", choice_str, sizeof(choice_str));
        int c = atoi(choice_str);

        switch (c) {
            case 1:  staff_view_pending_transfers(); break;
            case 2:  customer_view_transfer_status(); break;
            case 3:  staff_approve_transfer(); break;
            case 4:  staff_reject_transfer(); break;
            case 5:  staff_add_customer_account(); break;
            case 6:  staff_view_all_accounts(); break;
            case 7:  manager_change_account_status(); break;
            case 8:  staff_view_customer_tx(); break;
            case 9:  staff_view_pending_loans(); break;
            case 10: staff_process_loan(); break;
            case 11: manager_view_feedback(); break;
            case 12: do_change_password(); break;
            case 13: do_logout(); return;
            default: printf("[!] Invalid selection.\n"); break;
        }
    }
}

static void run_admin_menu(void) {
    while (g_user_id > 0) {
        printf("\n=======================================================\n");
        printf("             ADMINISTRATOR PORTAL - [%s]\n", g_username);
        printf("=======================================================\n");
        printf(" 1. Add Staff Member (Employee / Manager)\n");
        printf(" 2. View All System Users\n");
        printf(" 3. Modify User Role & Active Status\n");
        printf(" 4. Reset User Password\n");
        printf(" 5. View System Overview Statistics\n");
        printf(" 6. View Pending High-Value Transfers Queue\n");
        printf(" 7. View All Bank Accounts\n");
        printf(" 8. Change Password\n");
        printf(" 9. Logout\n");
        printf("=======================================================\n");

        char choice_str[16];
        read_str("Choose an option [1-9]: ", choice_str, sizeof(choice_str));
        int c = atoi(choice_str);

        switch (c) {
            case 1: admin_add_staff(); break;
            case 2: admin_view_all_users(); break;
            case 3: admin_modify_user(); break;
            case 4: admin_reset_password(); break;
            case 5: admin_view_stats(); break;
            case 6: staff_view_pending_transfers(); break;
            case 7: staff_view_all_accounts(); break;
            case 8: do_change_password(); break;
            case 9: do_logout(); return;
            default: printf("[!] Invalid selection.\n"); break;
        }
    }
}

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *host = "127.0.0.1";
    int port = DEFAULT_PORT;

    if (argc > 1) host = argv[1];
    if (argc > 2) port = atoi(argv[2]);

    printf("=========================================================\n");
    printf("   BANKING MANAGEMENT SYSTEM - CLIENT INTERFACE\n");
    printf("=========================================================\n");

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", WSAGetLastError());
        return 1;
    }
#endif

    g_sock = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &serv_addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid address / Address not supported: %s\n", host);
        close_socket(g_sock);
        return 1;
    }

    printf("[*] Connecting to banking server at %s:%d...\n", host, port);
    if (connect(g_sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("connect");
        fprintf(stderr, "[!] Ensure the banking server is running on port %d.\n", port);
        close_socket(g_sock);
        return 1;
    }

    printf("[+] Connected to server successfully!\n");

    while (1) {
        printf("\n=========================================\n");
        printf(" 1. Login\n");
        printf(" 2. Exit\n");
        printf("=========================================\n");
        char opt[16];
        read_str("Choose option: ", opt, sizeof(opt));
        if (atoi(opt) == 2) break;

        if (atoi(opt) == 1) {
            if (do_login()) {
                switch (g_role) {
                    case ROLE_CUSTOMER: run_customer_menu(); break;
                    case ROLE_EMPLOYEE: run_employee_menu(); break;
                    case ROLE_MANAGER:  run_manager_menu(); break;
                    case ROLE_ADMIN:    run_admin_menu(); break;
                    default: printf("[!] Unknown role.\n"); break;
                }
            }
        }
    }

    close_socket(g_sock);
#ifdef _WIN32
    WSACleanup();
#endif
    printf("[*] Goodbye!\n");
    return 0;
}
