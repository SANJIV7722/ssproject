#ifndef APPROVAL_H
#define APPROVAL_H

#include "models.h"

/* Error Codes for Approval Module */
#define APPROVAL_OK                     0
#define APPROVAL_ERR_NOT_AUTHORIZED     -1
#define APPROVAL_ERR_SELF_APPROVAL      -2
#define APPROVAL_ERR_INVALID_STATE      -3
#define APPROVAL_ERR_INSUFFICIENT_FUNDS -4
#define APPROVAL_ERR_ACCOUNT_INACTIVE   -5
#define APPROVAL_ERR_NOT_FOUND          -6
#define APPROVAL_ERR_SYSTEM             -7
#define APPROVAL_ERR_INVALID_PARAM      -8

/* High-value threshold submission
 * Returns:
 *   1 = Below threshold -> immediate transfer completed (*out_tx_id populated)
 *   2 = At or above threshold -> request created in PENDING_APPROVAL (*out_req_id populated)
 *   < 0 = Error code
 */
int submit_transfer_for_approval(int authenticated_user_id,
                                 int sender_account_id,
                                 int receiver_account_id,
                                 long long amount_cents,
                                 int *out_req_id,
                                 int *out_tx_id);

/* Approve pending transfer request by authorized reviewer (Manager / Employee)
 * Returns APPROVAL_OK on success, or negative error code.
 */
int approve_transfer_request(int authenticated_reviewer_id,
                             UserRole reviewer_role,
                             int request_id,
                             const char *resolution_note,
                             int *out_tx_id);

/* Reject pending transfer request by authorized reviewer
 * Returns APPROVAL_OK on success, or negative error code.
 */
int reject_transfer_request(int authenticated_reviewer_id,
                            UserRole reviewer_role,
                            int request_id,
                            const char *resolution_note);

/* Cancel pending transfer request by the requesting customer
 * Returns APPROVAL_OK on success, or negative error code.
 */
int cancel_transfer_request(int authenticated_user_id,
                            int request_id);

/* Get transfer request details with permission check */
int get_transfer_request_status(int authenticated_user_id,
                                UserRole role,
                                int request_id,
                                TransferRequest *out_req);

/* List all pending high-value transfer requests for authorized reviewers */
int list_pending_transfer_requests(int authenticated_reviewer_id,
                                   UserRole role,
                                   TransferRequest **out_reqs,
                                   int *out_count);

/* List all transfer requests submitted by a specific user */
int list_user_transfer_requests(int authenticated_user_id,
                                TransferRequest **out_reqs,
                                int *out_count);

#endif /* APPROVAL_H */
