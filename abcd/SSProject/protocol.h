#ifndef PROTOCOL_H
#define PROTOCOL_H

/* Response Status Prefix */
#define RESP_SUCCESS "SUCCESS"
#define RESP_ERROR   "ERROR"

/* Authentication Commands */
#define CMD_LOGIN                  "LOGIN"                  /* LOGIN|username|password */
#define CMD_LOGOUT                 "LOGOUT"                 /* LOGOUT */
#define CMD_CHANGE_PASSWORD        "CHANGE_PASSWORD"        /* CHANGE_PASSWORD|old_pass|new_pass */

/* Customer Commands */
#define CMD_GET_BALANCE            "GET_BALANCE"            /* GET_BALANCE */
#define CMD_DEPOSIT                "DEPOSIT"                /* DEPOSIT|amount_cents */
#define CMD_WITHDRAW               "WITHDRAW"               /* WITHDRAW|amount_cents */
#define CMD_TRANSFER               "TRANSFER"               /* TRANSFER|receiver_account_id|amount_cents */
#define CMD_VIEW_TRANSFER_STATUS   "VIEW_TRANSFER_STATUS"   /* VIEW_TRANSFER_STATUS|request_id */
#define CMD_CANCEL_TRANSFER        "CANCEL_TRANSFER"        /* CANCEL_TRANSFER|request_id */
#define CMD_APPLY_LOAN             "APPLY_LOAN"             /* APPLY_LOAN|amount_cents */
#define CMD_VIEW_LOANS             "VIEW_LOANS"             /* VIEW_LOANS */
#define CMD_SUBMIT_FEEDBACK        "SUBMIT_FEEDBACK"        /* SUBMIT_FEEDBACK|message */
#define CMD_VIEW_TRANSACTIONS      "VIEW_TRANSACTIONS"      /* VIEW_TRANSACTIONS */
#define CMD_MY_TRANSFERS           "MY_TRANSFERS"           /* MY_TRANSFERS */

/* Employee / Manager Commands */
#define CMD_ADD_ACCOUNT            "ADD_ACCOUNT"            /* ADD_ACCOUNT|username|initial_deposit_cents */
#define CMD_MODIFY_CUSTOMER        "MODIFY_CUSTOMER"        /* MODIFY_CUSTOMER|user_id|active_status */
#define CMD_VIEW_ALL_ACCOUNTS      "VIEW_ALL_ACCOUNTS"      /* VIEW_ALL_ACCOUNTS */
#define CMD_VIEW_PENDING_TRANSFERS "VIEW_PENDING_TRANSFERS" /* VIEW_PENDING_TRANSFERS */
#define CMD_VIEW_TRANSFER_DETAILS  "VIEW_TRANSFER_DETAILS"  /* VIEW_TRANSFER_DETAILS|request_id */
#define CMD_APPROVE_TRANSFER       "APPROVE_TRANSFER"       /* APPROVE_TRANSFER|request_id|resolution_note */
#define CMD_REJECT_TRANSFER        "REJECT_TRANSFER"        /* REJECT_TRANSFER|request_id|resolution_note */
#define CMD_VIEW_PENDING_LOANS     "VIEW_PENDING_LOANS"     /* VIEW_PENDING_LOANS */
#define CMD_PROCESS_LOAN           "PROCESS_LOAN"           /* PROCESS_LOAN|loan_id|action(APPROVE/REJECT) */
#define CMD_VIEW_CUSTOMER_TX       "VIEW_CUSTOMER_TX"       /* VIEW_CUSTOMER_TX|account_id */

/* Manager Exclusive Commands */
#define CMD_VIEW_FEEDBACK          "VIEW_FEEDBACK"          /* VIEW_FEEDBACK */
#define CMD_CHANGE_ACCOUNT_STATUS  "CHANGE_ACCOUNT_STATUS"  /* CHANGE_ACCOUNT_STATUS|account_id|active_status */

/* Admin Commands */
#define CMD_ADD_STAFF              "ADD_STAFF"              /* ADD_STAFF|username|password|role_str */
#define CMD_MODIFY_USER            "MODIFY_USER"            /* MODIFY_USER|user_id|role_str|active_status */
#define CMD_VIEW_ALL_USERS         "VIEW_ALL_USERS"         /* VIEW_ALL_USERS */
#define CMD_ADMIN_RESET_PASSWORD   "ADMIN_RESET_PASSWORD"   /* ADMIN_RESET_PASSWORD|user_id|new_password */
#define CMD_VIEW_SYSTEM_STATS      "VIEW_SYSTEM_STATS"      /* VIEW_SYSTEM_STATS */

#endif /* PROTOCOL_H */
