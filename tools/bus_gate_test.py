#!/usr/bin/env python3
"""Regression test for the bus hold: who may do what while the bus is held.

Runs against a live gateway. It homes module 0 a few times (a reel turns) and briefly turns
the Maintenance switch on and off; it writes nothing to any module's EEPROM.

    python3 tools/bus_gate_test.py 192.168.1.192

The matrix it pins down, and why each row exists:

  free bus              everything works, tagged or not
  operator hold         content from outside is refused 503, the dashboard's own is not,
   (maintenance,         and HOMING WORKS FOR EVERYONE -- issue #7: the Calibration tab's own
    calibration)         hold refused the operator's homing when the request tag was lost
  machine test hold     homing and content are both refused, for everyone: one extra frame
   (soak, probe)         invalidates what the test is measuring
  restore (exclusive)   everything is refused -- not exercised here, it rewrites EEPROM
  hold ownership        the client that took a lease counts as the operator afterwards even
                         without the tag, so a proxy that drops headers cannot lock it out
  hold lifetime         a lease expires by itself; cancelling a soak frees the bus before the
                         call returns -- every wait inside the run is cancel-aware, so this
                         holds even mid-way through a broadcast reply window, the longest one
  force release         an operator can always clear a stuck hold from the banner
"""
import json, sys, time, urllib.error, urllib.request

GW = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.192"
BASE = "http://%s" % GW
fails = []

def call(method, path, body=None, tag=False, timeout=20):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    if tag:
        req.add_header("X-SF-Client", "dashboard")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.loads(r.read().decode("latin-1") or "{}")
    except urllib.error.HTTPError as e:
        try:    return e.code, json.loads(e.read().decode("latin-1") or "{}")
        except Exception: return e.code, {}

def check(name, got, want):
    ok = got == want
    print("  %-58s %s (%s)" % (name, "PASS" if ok else "FAIL -> %s" % (got,), want))
    if not ok: fails.append(name)

def bus():            return call("GET", "/api/bus")[1]
def home(tag):        return call("POST", "/api/flap/home", {"id": 0}, tag=tag)[0]
def content(tag):     return call("POST", "/api/flap/char", {"id": 0, "char": " "}, tag=tag)[0]
def hold(reason, s):  return call("POST", "/api/bus/hold", {"reason": reason, "seconds": s}, tag=True)
def release(r, force=False): return call("POST", "/api/bus/release", {"reason": r, "force": force}, tag=True)
def maint(on):        return call("POST", "/api/maintenance", {"on": on}, tag=True)

def wait_free(limit=20):
    for _ in range(limit * 2):
        if bus().get("level", 0) == 0: return True
        time.sleep(0.5)
    return False

print("Gateway %s -- firmware %s" % (GW, call("GET", "/api/config")[1].get("version")))
if not wait_free():
    print("the bus is held by %r; not starting" % bus().get("reason")); sys.exit(2)

print("\n1. free bus")
check("home, untagged", home(False), 200)
check("home, tagged", home(True), 200)
check("content, untagged", content(False), 200)

print("\n2. operator hold (the manual Maintenance switch, owned by the firmware)")
maint(True)
check("bus level is busy", bus().get("level"), 1)
check("content from outside is refused", content(False), 503)
check("content from the dashboard works", content(True), 200)
check("HOMING WORKS UNTAGGED (issue #7)", home(False), 200)
check("homing works tagged", home(True), 200)
maint(False)
check("switch off frees the bus", bus().get("level"), 0)

print("\n3. a lease taken over REST")
hold("backup", 10)
check("bus level is busy", bus().get("level"), 1)
check("the holder is the operator, with no tag", content(False), 200)
check("homing works", home(False), 200)
release("backup")
check("released", bus().get("level"), 0)
hold("backup", 5)
print("   waiting for the lease to lapse...")
time.sleep(8)
check("a lease nobody renews expires", bus().get("level"), 0)

print("\n4. a machine-run test holds the bus (soak)")
st, j = call("POST", "/api/flap/soak", {"minutes": 1})
check("soak started", (st, j.get("started")), (200, True))
time.sleep(3)
check("bus level is busy", bus().get("level"), 1)
check("content is refused even from the dashboard", content(True), 503)
check("homing is refused, tagged", home(True), 503)
check("homing is refused, untagged", home(False), 503)
check("another test is refused", call("POST", "/api/flap/probe", {"id": 0}, tag=True)[0], 409)

print("\n5. cancelling frees the bus promptly (the stuck-hold report)")
t0 = time.time()
st, j = call("POST", "/api/flap/soak/cancel", tag=True)
took = time.time() - t0
check("cancel accepted", st, 200)
check("the reply itself says the bus is free", j.get("busFree"), True)
check("cancel answered in under 2 s", took < 2, True)
print("   it took %.0f ms, firmware measured %s ms" % (took * 1000, j.get("ms")))
check("bus free", bus().get("level"), 0)
check("the soak reports itself cancelled", call("GET", "/api/flap/soak/status")[1].get("state"), "cancelled")

print("\n6. an operator can always clear a stuck hold")
maint(True)
check("force release clears it", release("maintenance", force=True)[0], 200)
check("bus free", bus().get("level"), 0)
check("a restore can never be force-released", release("restore", force=True)[0], 409)
maint(False)

print("\n%s" % ("ALL PASSED" if not fails else "FAILED: " + "; ".join(fails)))
sys.exit(1 if fails else 0)
