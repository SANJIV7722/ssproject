#ifndef MODELS_H
#define MODELS_H

#include <time.h>

#define DEFAULT_PORT 8080
#define BUFFER_SIZE 4096
#define MAX_LINE_LEN 1024
#define MAX_PATH_LEN 512

/*
 * High-value transfer threshold in cents.
 * 10,000,000 cents = $100,000.00 minor units.
 * Transfers >= threshold require dual authorization by an employee or manager.
 */
#define HIGH_VALUE_TRANSFER_THRESHOLD_CENTS 10000000LL

/* User Roles */
typedef enum {
    ROLE_UNKNOWN = 0,
    ROLE_CUSTOMER = 1,
    ROLE_EMPLOYEE = 2,
    ROLE_MANAGER = 3,
    ROLE_ADMIN = 4
} UserRole;

/* Transfer Request Status */
typedef enum {
    REQ_UNKNOWN = 0,
    REQ_PENDING_APPROVAL,
    REQ_APPROVED,
    REQ_COMPLETED,
    REQ_REJECTED,
    REQ_CANCELLED,
    REQ_FAILED
} TransferRequestStatus;

/* Loan Status */
typedef enum {
    LOAN_PENDING = 1,
    LOAN_APPROVED,
    LOAN_REJECTED
} LoanStatus;

/* User Record (users.txt)
 * user_id|username|password_hash|role|active
 */
typedef struct {
    int user_id;
    char username[64];
    char password_hash[64];
    UserRole role;
    int active; // 1 = active, 0 = inactive
} User;

/* Account Record (accounts.txt)
 * account_id|user_id|balance_cents|active
 */
typedef struct {
    int account_id;
    int user_id;
    long long balance_cents;
    int active; // 1 = active, 0 = inactive
} Account;

/* Transaction Record (transactions.txt)
 * transaction_id|account_id|type|amount_cents|related_account_id|details|timestamp
 */
typedef struct {
    int transaction_id;
    int account_id;
    char type[32]; // DEPOSIT, WITHDRAW, TRANSFER_OUT, TRANSFER_IN, LOAN_DISBURSEMENT
    long long amount_cents;
    int related_account_id; // -1 if none
    char details[128];
    time_t timestamp;
} Transaction;

/* Loan Record (loans.txt)
 * loan_id|account_id|amount_cents|status|assigned_employee_id|created_at|processed_at
 */
typedef struct {
    int loan_id;
    int account_id;
    long long amount_cents;
    char status[32]; // PENDING, APPROVED, REJECTED
    int assigned_employee_id;
    time_t created_at;
    time_t processed_at;
} Loan;

/* Feedback Record (feedback.txt)
 * feedback_id|user_id|message|timestamp
 */
typedef struct {
    int feedback_id;
    int user_id;
    char message[256];
    time_t timestamp;
} Feedback;

/* Transfer Request Record (transfer_requests.txt)
 * request_id|sender_account_id|receiver_account_id|amount_cents|requested_by_user_id|status|created_at|reviewed_by_user_id|reviewed_at|resolution_note|transaction_id
 */
typedef struct {
    int request_id;
    int sender_account_id;
    int receiver_account_id;
    long long amount_cents;
    int requested_by_user_id;
    char status[32]; // PENDING_APPROVAL, APPROVED, COMPLETED, REJECTED, CANCELLED, FAILED
    time_t created_at;
    int reviewed_by_user_id; // 0 if not yet reviewed
    time_t reviewed_at;
    char resolution_note[256];
    int transaction_id; // 0 if not completed
} TransferRequest;

/* Helper conversion functions */
const char* role_to_string(UserRole role);
UserRole string_to_role(const char* str);
const char* req_status_to_string(TransferRequestStatus status);
TransferRequestStatus string_to_req_status(const char* str);

#endif /* MODELS_H */
