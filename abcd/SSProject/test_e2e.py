import socket
import subprocess
import time
import os
import sys

PORT = 8088
SERVER_PROC = None

def start_server():
    global SERVER_PROC
    # Clear data directory if needed or let server init
    if os.path.exists("data/bank.lock"):
        try:
            if os.path.exists("data/sessions"):
                for f in os.listdir("data/sessions"):
                    os.remove(os.path.join("data/sessions", f))
        except Exception:
            pass

    server_bin = os.path.abspath("server.exe")
    env = os.environ.copy()
    env["PATH"] = "C:\\Program Files (x86)\\Dev-Cpp\\MinGW64\\bin;" + env.get("PATH", "")
    SERVER_PROC = subprocess.Popen([server_bin, str(PORT)], cwd=os.path.dirname(server_bin), env=env)
    
    # Wait for server port to be ready
    for _ in range(20):
        time.sleep(0.2)
        if SERVER_PROC.poll() is not None:
            print(f"[!] Server exited early with code {SERVER_PROC.returncode}")
            sys.exit(1)
        try:
            test_s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            test_s.connect(("127.0.0.1", PORT))
            test_s.close()
            print(f"[+] Server started and listening on port {PORT} with PID {SERVER_PROC.pid}")
            return
        except Exception:
            pass

    print("[!] Failed to connect to server after 4 seconds.")
    sys.exit(1)

def stop_server():
    global SERVER_PROC
    if SERVER_PROC:
        SERVER_PROC.terminate()
        try:
            SERVER_PROC.wait(timeout=3)
        except Exception:
            SERVER_PROC.kill()
        print("[+] Server stopped.")

class BankClient:
    def __init__(self, port=PORT):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.connect(("127.0.0.1", port))
        self.reader = self.sock.makefile("r", encoding="utf-8")

    def send(self, cmd):
        self.sock.sendall((cmd + "\n").encode("utf-8"))

    def recv(self):
        line = self.reader.readline()
        if not line:
            return ""
        return line.strip()

    def close(self):
        try:
            self.send("LOGOUT")
            self.recv()
        except Exception:
            pass
        self.sock.close()

def run_tests():
    print("==================================================")
    print("      RUNNING BANKING SYSTEM E2E TEST SUITE       ")
    print("==================================================")
    
    # ---------------- TEST 1: Login & Single Session Enforcement ----------------
    print("\n[Test 1] Testing Login & Single Active Session Enforcement...")
    c1 = BankClient()
    c1.send("LOGIN|alice|alice123")
    r1 = c1.recv()
    assert "SUCCESS|LOGIN_OK" in r1, f"Expected successful login, got {r1}"
    print("  -> Terminal 1: Alice logged in successfully.")

    # Attempt second login on different terminal
    c2 = BankClient()
    c2.send("LOGIN|alice|alice123")
    r2 = c2.recv()
    assert "ERROR|ALREADY_LOGGED_IN" in r2, f"Expected ALREADY_LOGGED_IN error, got {r2}"
    print("  -> Terminal 2: Duplicate session correctly rejected (Single session enforced).")
    c2.close()

    # ---------------- TEST 2: Balance Check ----------------
    print("\n[Test 2] Testing Balance Check for Alice (Account 1001)...")
    c1.send("GET_BALANCE")
    r_bal = c1.recv()
    assert "SUCCESS|BALANCE|1001|50000000|1" in r_bal, f"Expected Alice initial balance 50000000, got {r_bal}"
    print(f"  -> Alice initial balance: $500,000.00 (50,000,000 cents) confirmed.")

    # ---------------- TEST 3: Normal Transfer Below Threshold ($500.00) ----------------
    print("\n[Test 3] Testing Normal Transfer Below Threshold ($500.00 = 50,000 cents)...")
    # Alice transfers $500.00 to Bob (Account 1002)
    c1.send("TRANSFER|1002|50000")
    r_tr1 = c1.recv()
    assert "SUCCESS|TRANSFER_COMPLETED" in r_tr1, f"Expected immediate TRANSFER_COMPLETED, got {r_tr1}"
    print(f"  -> Response: {r_tr1}")

    # Check Alice new balance: 50,000,000 - 50,000 = 49,950,000
    c1.send("GET_BALANCE")
    r_bal_after = c1.recv()
    assert "49950000" in r_bal_after, f"Expected 49950000 balance, got {r_bal_after}"
    print("  -> Alice balance updated to $499,500.00 immediately.")

    # ---------------- TEST 4: High-Value Transfer Submission (>= $100,000.00) ----------------
    print("\n[Test 4] Testing High-Value Transfer (Threshold >= $100,000.00 = 10,000,000 cents)...")
    # Alice submits $150,000.00 (15,000,000 cents) transfer to Bob (Account 1002)
    c1.send("TRANSFER|1002|15000000")
    r_hv = c1.recv()
    assert "SUCCESS|APPROVAL_PENDING" in r_hv, f"Expected APPROVAL_PENDING, got {r_hv}"
    req_id = int(r_hv.split("|")[2])
    print(f"  -> High-value transfer queued for dual auth! Request ID: #{req_id}")

    # Verify that Alice's balance did NOT change!
    c1.send("GET_BALANCE")
    r_bal_check = c1.recv()
    assert "49950000" in r_bal_check, f"Balance should NOT change before approval! Got {r_bal_check}"
    print("  -> Verified: Balance remained unchanged ($499,500.00) upon submission.")

    # ---------------- TEST 5: Anti-Self-Approval Check (Staff Self-Approval) ----------------
    print("\n[Test 5] Testing Anti-Self-Approval Security Rule (Staff Self-Approval)...")
    # Alice (customer) attempts to approve her own request
    c1.send(f"APPROVE_TRANSFER|{req_id}|Self approving")
    r_self = c1.recv()
    assert "ERROR" in r_self, f"Requester should NOT be allowed to approve! Got {r_self}"
    print(f"  -> Alice blocked from approving: {r_self}")

    # Now test an actual staff member (employee1) who submits a high-value transfer and tries to approve it!
    # Manager opens an account for employee1 with $200,000
    c_mgr_init = BankClient()
    c_mgr_init.send("LOGIN|manager1|mgr123")
    c_mgr_init.recv()
    c_mgr_init.send("ADD_ACCOUNT|employee1|20000000") # $200,000.00
    r_acc = c_mgr_init.recv()
    assert "SUCCESS|ACCOUNT_CREATED" in r_acc
    emp_acc_id = int(r_acc.split("|")[2])
    c_mgr_init.close()

    # Employee1 logs in and submits a $120,000 transfer to Bob (1002)
    c_emp1 = BankClient()
    c_emp1.send("LOGIN|employee1|emp123")
    c_emp1.recv()
    c_emp1.send("TRANSFER|1002|12000000")
    r_emp_tr = c_emp1.recv()
    assert "SUCCESS|APPROVAL_PENDING" in r_emp_tr
    emp_req_id = int(r_emp_tr.split("|")[2])

    # Employee1 now attempts to approve their own request!
    c_emp1.send(f"APPROVE_TRANSFER|{emp_req_id}|Employee approving own transfer")
    r_self_emp = c_emp1.recv()
    assert "SELF_APPROVAL_NOT_ALLOWED" in r_self_emp, f"Expected SELF_APPROVAL_NOT_ALLOWED, got {r_self_emp}"
    print(f"  -> Strict Anti-Self-Approval enforced for staff: {r_self_emp}")
    c_emp1.close()

    # ---------------- TEST 6: Unauthorized Role Approval Check ----------------
    print("\n[Test 6] Testing Unauthorized Customer Approval Check...")
    # Bob (customer) logs in and tries to approve
    c_bob = BankClient()
    c_bob.send("LOGIN|bob|bob123")
    assert "SUCCESS" in c_bob.recv()
    c_bob.send(f"APPROVE_TRANSFER|{req_id}|Bob approving")
    r_unauth = c_bob.recv()
    assert "NOT_AUTHORIZED" in r_unauth or "ERROR" in r_unauth, f"Bob should be unauthorized, got {r_unauth}"
    print(f"  -> Customer blocked from approving: {r_unauth}")
    c_bob.close()

    # ---------------- TEST 7: Authorized Approval by Manager ----------------
    print("\n[Test 7] Testing Authorized Approval by Manager (manager1)...")
    c_mgr = BankClient()
    c_mgr.send("LOGIN|manager1|mgr123")
    assert "SUCCESS" in c_mgr.recv()

    # View pending transfers
    c_mgr.send("VIEW_PENDING_TRANSFERS")
    r_list = c_mgr.recv()
    assert "SUCCESS|PENDING_TRANSFERS" in r_list
    items = []
    while True:
        it = c_mgr.recv()
        if it == "END": break
        items.append(it)
    print(f"  -> Manager viewed queue: {len(items)} pending transfer(s) awaiting approval.")
    assert any(f"|{req_id}|" in x for x in items), f"Request #{req_id} should be in pending queue!"

    # Approve request
    c_mgr.send(f"APPROVE_TRANSFER|{req_id}|High value transfer authorized by Branch Manager")
    r_app = c_mgr.recv()
    assert "SUCCESS|TRANSFER_APPROVED" in r_app, f"Expected successful approval, got {r_app}"
    print(f"  -> Transfer approved by manager: {r_app}")
    c_mgr.close()

    # Verify Alice's balance is now debited: 49,950,000 - 15,000,000 = 34,950,000 ($349,500.00)
    c1.send("GET_BALANCE")
    r_alice_final = c1.recv()
    assert "34950000" in r_alice_final, f"Expected Alice balance 34950000, got {r_alice_final}"
    print("  -> Verified: Alice balance debited to $349,500.00 upon approval.")

    # Verify Bob's balance is credited: 20,000,000 + 50,000 + 15,000,000 = 35,050,000 ($350,500.00)
    c_bob2 = BankClient()
    c_bob2.send("LOGIN|bob|bob123")
    c_bob2.recv()
    c_bob2.send("GET_BALANCE")
    r_bob_bal = c_bob2.recv()
    assert "35050000" in r_bob_bal, f"Expected Bob balance 35050000, got {r_bob_bal}"
    print("  -> Verified: Bob balance credited to $350,500.00 upon approval.")
    c_bob2.close()

    # ---------------- TEST 8: Rejection Workflow ----------------
    print("\n[Test 8] Testing High-Value Transfer Rejection Workflow...")
    # Alice submits another high-value transfer ($120,000.00 = 12,000,000 cents)
    c1.send("TRANSFER|1002|12000000")
    r_hv2 = c1.recv()
    assert "SUCCESS|APPROVAL_PENDING" in r_hv2
    req_id2 = int(r_hv2.split("|")[2])

    # Manager rejects it
    c_mgr2 = BankClient()
    c_mgr2.send("LOGIN|manager1|mgr123")
    c_mgr2.recv()
    c_mgr2.send(f"REJECT_TRANSFER|{req_id2}|Suspected anomaly - rejected")
    r_rej = c_mgr2.recv()
    assert "SUCCESS|TRANSFER_REJECTED" in r_rej, f"Expected rejection, got {r_rej}"
    print(f"  -> Transfer Request #{req_id2} rejected by Manager.")
    c_mgr2.close()

    # Verify Alice's balance is completely UNTOUCHED
    c1.send("GET_BALANCE")
    r_bal_rej = c1.recv()
    assert "34950000" in r_bal_rej, f"Balance should remain 34950000, got {r_bal_rej}"
    print("  -> Verified: Balances unchanged after rejection.")

    # ---------------- TEST 9: Cancellation Workflow ----------------
    print("\n[Test 9] Testing Customer Cancellation Workflow...")
    # Alice submits $100,000.00 transfer
    c1.send("TRANSFER|1002|10000000")
    r_hv3 = c1.recv()
    req_id3 = int(r_hv3.split("|")[2])
    print(f"  -> Alice created Request #{req_id3}")

    # Alice cancels it
    c1.send(f"CANCEL_TRANSFER|{req_id3}")
    r_can = c1.recv()
    assert "SUCCESS|TRANSFER_CANCELLED" in r_can, f"Expected cancellation, got {r_can}"
    print(f"  -> Request #{req_id3} cancelled by Alice.")

    # Manager cannot approve cancelled request
    c_emp = BankClient()
    c_emp.send("LOGIN|employee1|emp123")
    c_emp.recv()
    c_emp.send(f"APPROVE_TRANSFER|{req_id3}|Trying to approve cancelled")
    r_app_can = c_emp.recv()
    assert "INVALID_STATE" in r_app_can or "ERROR" in r_app_can, f"Cancelled request cannot be approved! Got {r_app_can}"
    print("  -> Verified: Cancelled request cannot be approved.")
    c_emp.close()

    # ---------------- TEST 10: Loans & Feedback ----------------
    print("\n[Test 10] Testing Loan Application & Feedback submission...")
    c1.send("APPLY_LOAN|500000") # $5,000.00 loan
    r_loan = c1.recv()
    assert "SUCCESS|LOAN_APPLIED" in r_loan
    print(f"  -> Loan applied: {r_loan}")

    c1.send("SUBMIT_FEEDBACK|Excellent banking experience with dual auth!")
    r_fb = c1.recv()
    assert "SUCCESS|FEEDBACK_SUBMITTED" in r_fb
    print(f"  -> Feedback submitted: {r_fb}")

    c1.close()

    print("\n==================================================")
    print("        ALL 10 VERIFICATION TESTS PASSED!         ")
    print("==================================================")

if __name__ == "__main__":
    try:
        start_server()
        run_tests()
    finally:
        stop_server()
