#include <string.h>
#include "models.h"

const char* role_to_string(UserRole role) {
    switch (role) {
        case ROLE_CUSTOMER: return "CUSTOMER";
        case ROLE_EMPLOYEE: return "EMPLOYEE";
        case ROLE_MANAGER:  return "MANAGER";
        case ROLE_ADMIN:    return "ADMIN";
        default:            return "UNKNOWN";
    }
}

UserRole string_to_role(const char* str) {
    if (!str) return ROLE_UNKNOWN;
    if (strcmp(str, "CUSTOMER") == 0) return ROLE_CUSTOMER;
    if (strcmp(str, "EMPLOYEE") == 0) return ROLE_EMPLOYEE;
    if (strcmp(str, "MANAGER") == 0)  return ROLE_MANAGER;
    if (strcmp(str, "ADMIN") == 0)    return ROLE_ADMIN;
    return ROLE_UNKNOWN;
}

const char* req_status_to_string(TransferRequestStatus status) {
    switch (status) {
        case REQ_PENDING_APPROVAL: return "PENDING_APPROVAL";
        case REQ_APPROVED:         return "APPROVED";
        case REQ_COMPLETED:        return "COMPLETED";
        case REQ_REJECTED:         return "REJECTED";
        case REQ_CANCELLED:        return "CANCELLED";
        case REQ_FAILED:           return "FAILED";
        default:                   return "UNKNOWN";
    }
}

TransferRequestStatus string_to_req_status(const char* str) {
    if (!str) return REQ_UNKNOWN;
    if (strcmp(str, "PENDING_APPROVAL") == 0) return REQ_PENDING_APPROVAL;
    if (strcmp(str, "APPROVED") == 0)         return REQ_APPROVED;
    if (strcmp(str, "COMPLETED") == 0)        return REQ_COMPLETED;
    if (strcmp(str, "REJECTED") == 0)         return REQ_REJECTED;
    if (strcmp(str, "CANCELLED") == 0)        return REQ_CANCELLED;
    if (strcmp(str, "FAILED") == 0)           return REQ_FAILED;
    return REQ_UNKNOWN;
}
