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
#include <time.h>
#include "approval.h"
#include "storage.h"

int submit_transfer_for_approval(int authenticated_user_id,
                                 int sender_account_id,
                                 int receiver_account_id,
                                 long long amount_cents,
                                 int *out_req_id,
                                 int *out_tx_id) {
    if (sender_account_id == receiver_account_id || amount_cents <= 0) {
        return APPROVAL_ERR_INVALID_PARAM;
    }

    if (storage_lock_write() != 0) return APPROVAL_ERR_SYSTEM;

    Account sender, receiver;
    if (storage_find_account_by_id(sender_account_id, &sender) != 0) {
        storage_unlock();
        return APPROVAL_ERR_NOT_FOUND;
    }
    if (storage_find_account_by_id(receiver_account_id, &receiver) != 0) {
        storage_unlock();
        return APPROVAL_ERR_NOT_FOUND;
    }

    /* Check sender ownership */
    if (sender.user_id != authenticated_user_id) {
        storage_unlock();
        return APPROVAL_ERR_NOT_AUTHORIZED;
    }

    if (!sender.active || !receiver.active) {
        storage_unlock();
        return APPROVAL_ERR_ACCOUNT_INACTIVE;
    }

    if (sender.balance_cents < amount_cents) {
        storage_unlock();
        return APPROVAL_ERR_INSUFFICIENT_FUNDS;
    }

    /* Check Threshold Rule:
     * If amount < threshold -> execute normal transfer immediately.
     * If amount >= threshold -> create PENDING_APPROVAL request, do NOT touch balances.
     */
    if (amount_cents < HIGH_VALUE_TRANSFER_THRESHOLD_CENTS) {
        int tx_id = 0;
        int res = storage_execute_transfer(sender_account_id, receiver_account_id, amount_cents, "Direct Transfer", &tx_id);
        storage_unlock();
        if (res == 0) {
            if (out_tx_id) *out_tx_id = tx_id;
            return 1; /* Completed immediately */
        }
        return APPROVAL_ERR_SYSTEM;
    } else {
        int req_id = 0;
        int res = storage_add_transfer_request(sender_account_id, receiver_account_id,
                                             amount_cents, authenticated_user_id, &req_id);
        storage_unlock();
        if (res == 0) {
            if (out_req_id) *out_req_id = req_id;
            return 2; /* Queued for dual authorization */
        }
        return APPROVAL_ERR_SYSTEM;
    }
}

int approve_transfer_request(int authenticated_reviewer_id,
                             UserRole reviewer_role,
                             int request_id,
                             const char *resolution_note,
                             int *out_tx_id) {
    /* Only Employees and Managers (or Admin) can approve */
    if (reviewer_role != ROLE_EMPLOYEE && reviewer_role != ROLE_MANAGER && reviewer_role != ROLE_ADMIN) {
        return APPROVAL_ERR_NOT_AUTHORIZED;
    }

    if (storage_lock_write() != 0) return APPROVAL_ERR_SYSTEM;

    TransferRequest req;
    if (storage_find_transfer_request_by_id(request_id, &req) != 0) {
        storage_unlock();
        return APPROVAL_ERR_NOT_FOUND;
    }

    /* Check state */
    if (strcmp(req.status, "PENDING_APPROVAL") != 0) {
        storage_unlock();
        return APPROVAL_ERR_INVALID_STATE;
    }

    /* Anti-Self-Approval Rule: The requesting customer cannot approve their own transfer! */
    if (req.requested_by_user_id == authenticated_reviewer_id) {
        storage_unlock();
        return APPROVAL_ERR_SELF_APPROVAL;
    }

    /* Revalidate accounts and current available balance under lock */
    Account sender, receiver;
    if (storage_find_account_by_id(req.sender_account_id, &sender) != 0 ||
        storage_find_account_by_id(req.receiver_account_id, &receiver) != 0) {
        storage_unlock();
        return APPROVAL_ERR_NOT_FOUND;
    }

    if (!sender.active || !receiver.active) {
        storage_unlock();
        return APPROVAL_ERR_ACCOUNT_INACTIVE;
    }

    if (sender.balance_cents < req.amount_cents) {
        /* Mark as FAILED or return INSUFFICIENT_FUNDS */
        strncpy(req.status, "FAILED", sizeof(req.status) - 1);
        req.reviewed_by_user_id = authenticated_reviewer_id;
        req.reviewed_at = time(NULL);
        snprintf(req.resolution_note, sizeof(req.resolution_note), "Auto-failed: Insufficient balance at approval time");
        storage_update_transfer_request(&req);
        storage_unlock();
        return APPROVAL_ERR_INSUFFICIENT_FUNDS;
    }

    /* Execute the transfer atomically */
    int tx_id = 0;
    char details[128];
    snprintf(details, sizeof(details), "Dual Auth Transfer (Req #%d)", req.request_id);
    int res = storage_execute_transfer(req.sender_account_id, req.receiver_account_id,
                                      req.amount_cents, details, &tx_id);
    if (res != 0) {
        storage_unlock();
        return APPROVAL_ERR_SYSTEM;
    }

    /* Update request record to COMPLETED */
    strncpy(req.status, "COMPLETED", sizeof(req.status) - 1);
    req.reviewed_by_user_id = authenticated_reviewer_id;
    req.reviewed_at = time(NULL);
    if (resolution_note && strlen(resolution_note) > 0) {
        strncpy(req.resolution_note, resolution_note, sizeof(req.resolution_note) - 1);
    } else {
        strncpy(req.resolution_note, "Approved", sizeof(req.resolution_note) - 1);
    }
    req.transaction_id = tx_id;

    storage_update_transfer_request(&req);
    storage_unlock();

    if (out_tx_id) *out_tx_id = tx_id;
    return APPROVAL_OK;
}

int reject_transfer_request(int authenticated_reviewer_id,
                            UserRole reviewer_role,
                            int request_id,
                            const char *resolution_note) {
    if (reviewer_role != ROLE_EMPLOYEE && reviewer_role != ROLE_MANAGER && reviewer_role != ROLE_ADMIN) {
        return APPROVAL_ERR_NOT_AUTHORIZED;
    }

    if (storage_lock_write() != 0) return APPROVAL_ERR_SYSTEM;

    TransferRequest req;
    if (storage_find_transfer_request_by_id(request_id, &req) != 0) {
        storage_unlock();
        return APPROVAL_ERR_NOT_FOUND;
    }

    if (strcmp(req.status, "PENDING_APPROVAL") != 0) {
        storage_unlock();
        return APPROVAL_ERR_INVALID_STATE;
    }

    strncpy(req.status, "REJECTED", sizeof(req.status) - 1);
    req.reviewed_by_user_id = authenticated_reviewer_id;
    req.reviewed_at = time(NULL);
    if (resolution_note && strlen(resolution_note) > 0) {
        strncpy(req.resolution_note, resolution_note, sizeof(req.resolution_note) - 1);
    } else {
        strncpy(req.resolution_note, "Rejected by reviewer", sizeof(req.resolution_note) - 1);
    }

    storage_update_transfer_request(&req);
    storage_unlock();

    return APPROVAL_OK;
}

int cancel_transfer_request(int authenticated_user_id, int request_id) {
    if (storage_lock_write() != 0) return APPROVAL_ERR_SYSTEM;

    TransferRequest req;
    if (storage_find_transfer_request_by_id(request_id, &req) != 0) {
        storage_unlock();
        return APPROVAL_ERR_NOT_FOUND;
    }

    /* Only the requester can cancel their request */
    if (req.requested_by_user_id != authenticated_user_id) {
        storage_unlock();
        return APPROVAL_ERR_NOT_AUTHORIZED;
    }

    if (strcmp(req.status, "PENDING_APPROVAL") != 0) {
        storage_unlock();
        return APPROVAL_ERR_INVALID_STATE;
    }

    strncpy(req.status, "CANCELLED", sizeof(req.status) - 1);
    req.reviewed_at = time(NULL);
    strncpy(req.resolution_note, "Cancelled by user", sizeof(req.resolution_note) - 1);

    storage_update_transfer_request(&req);
    storage_unlock();

    return APPROVAL_OK;
}

int get_transfer_request_status(int authenticated_user_id,
                                UserRole role,
                                int request_id,
                                TransferRequest *out_req) {
    if (storage_lock_read() != 0) return APPROVAL_ERR_SYSTEM;

    TransferRequest req;
    if (storage_find_transfer_request_by_id(request_id, &req) != 0) {
        storage_unlock();
        return APPROVAL_ERR_NOT_FOUND;
    }

    /* Authorization check: Customer can only view their own request. Staff can view all. */
    if (role == ROLE_CUSTOMER && req.requested_by_user_id != authenticated_user_id) {
        storage_unlock();
        return APPROVAL_ERR_NOT_AUTHORIZED;
    }

    storage_unlock();
    if (out_req) *out_req = req;
    return APPROVAL_OK;
}

int list_pending_transfer_requests(int authenticated_reviewer_id,
                                   UserRole role,
                                   TransferRequest **out_reqs,
                                   int *out_count) {
    (void)authenticated_reviewer_id;
    if (role != ROLE_EMPLOYEE && role != ROLE_MANAGER && role != ROLE_ADMIN) {
        return APPROVAL_ERR_NOT_AUTHORIZED;
    }

    if (storage_lock_read() != 0) return APPROVAL_ERR_SYSTEM;

    TransferRequest *all = NULL;
    int total = 0;
    storage_get_all_transfer_requests(&all, &total);

    int capacity = 16;
    int count = 0;
    TransferRequest *pending = (TransferRequest*)malloc(sizeof(TransferRequest) * capacity);

    for (int i = 0; i < total; i++) {
        if (strcmp(all[i].status, "PENDING_APPROVAL") == 0) {
            if (count >= capacity) {
                capacity *= 2;
                pending = (TransferRequest*)realloc(pending, sizeof(TransferRequest) * capacity);
            }
            pending[count++] = all[i];
        }
    }

    free(all);
    storage_unlock();

    *out_reqs = pending;
    *out_count = count;
    return APPROVAL_OK;
}

int list_user_transfer_requests(int authenticated_user_id,
                                TransferRequest **out_reqs,
                                int *out_count) {
    if (storage_lock_read() != 0) return APPROVAL_ERR_SYSTEM;

    TransferRequest *all = NULL;
    int total = 0;
    storage_get_all_transfer_requests(&all, &total);

    int capacity = 16;
    int count = 0;
    TransferRequest *mine = (TransferRequest*)malloc(sizeof(TransferRequest) * capacity);

    for (int i = 0; i < total; i++) {
        if (all[i].requested_by_user_id == authenticated_user_id) {
            if (count >= capacity) {
                capacity *= 2;
                mine = (TransferRequest*)realloc(mine, sizeof(TransferRequest) * capacity);
            }
            mine[count++] = all[i];
        }
    }

    free(all);
    storage_unlock();

    *out_reqs = mine;
    *out_count = count;
    return APPROVAL_OK;
}
