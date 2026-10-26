#ifndef STORAGE_H
#define STORAGE_H

#include "models.h"

/* Initialize data directory, lock file, and initial seed users */
int storage_init(void);

/* POSIX File Locking on data/bank.lock */
int storage_lock_read(void);
int storage_lock_write(void);
int storage_unlock(void);

/* Single Active Session Enforcement */
int session_acquire(int user_id);
int session_release(int user_id);
void session_cleanup_all(void);

/* User Storage */
int storage_find_user_by_username(const char *username, User *out_user);
int storage_find_user_by_id(int user_id, User *out_user);
int storage_add_user(const char *username, const char *password_hash, UserRole role, int *out_user_id);
int storage_update_user(const User *user);
int storage_get_all_users(User **out_users, int *out_count);

/* Account Storage */
int storage_find_account_by_id(int account_id, Account *out_account);
int storage_find_account_by_user_id(int user_id, Account *out_account);
int storage_add_account(int user_id, long long initial_balance_cents, int *out_account_id);
int storage_update_account(const Account *account);
int storage_get_all_accounts(Account **out_accounts, int *out_count);

/* Transaction Storage */
int storage_add_transaction(int account_id, const char *type, long long amount_cents,
                            int related_account_id, const char *details, int *out_tx_id);
int storage_get_transactions_by_account(int account_id, Transaction **out_txs, int *out_count);
int storage_get_all_transactions(Transaction **out_txs, int *out_count);

/* Loan Storage */
int storage_add_loan(int account_id, long long amount_cents, int *out_loan_id);
int storage_find_loan_by_id(int loan_id, Loan *out_loan);
int storage_update_loan(const Loan *loan);
int storage_get_all_loans(Loan **out_loans, int *out_count);

/* Feedback Storage */
int storage_add_feedback(int user_id, const char *message, int *out_feedback_id);
int storage_get_all_feedback(Feedback **out_feedbacks, int *out_count);

/* Transfer Request Storage */
int storage_add_transfer_request(int sender_account_id, int receiver_account_id,
                                 long long amount_cents, int requested_by_user_id,
                                 int *out_request_id);
int storage_find_transfer_request_by_id(int request_id, TransferRequest *out_request);
int storage_update_transfer_request(const TransferRequest *request);
int storage_get_all_transfer_requests(TransferRequest **out_requests, int *out_count);

/* Trusted Atomic Transfer Execution (Called by both direct transfer & approval) */
int storage_execute_transfer(int sender_account_id, int receiver_account_id,
                             long long amount_cents, const char *details,
                             int *out_tx_id);

#endif /* STORAGE_H */
