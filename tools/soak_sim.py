#!/usr/bin/env python3
"""A flaky bus, simulated -- for working on the soak-test report without a wall to break.

Produces the exact JSON GET /api/flap/soak/status returns, by running the firmware's pass
schedule (V v Q d A B W X, see modules.h) against a model of a wall with faults injected:

  term      the line loses long frames: the chance a reply is lost grows with its length,
            and is worse for the modules listed in `far` (the ones at the far end)
  contact   a module with a bad pogo pin / sagging rail: misses at every size, in bursts
  dup       two boards share this ID: broadcasts get two replies (one window counts twice),
            direct queries collide and arrive corrupt or not at all
  silent    never answers
  eeprom    writes do not land (the read-back shows the old value)
  nodump    the module's map has grown too big for its firmware to send: 'v'/'Q' always,
            'd'/'A' never (the field report "stopped answering mXA; erasing EEPROM fixed it")
  stray     extra replies in roll calls from IDs the table does not list

Usage:  python3 tools/soak_sim.py <scenario> [minutes] [seed]     (scenario: see SCENARIOS, or 'all')
        prints the JSON; with 'all', writes one file per scenario into the directory given as $OUT.
"""
import json, os, random, sys

STEPS = ["v", "Q", "d", "A", "W", "V", "B"]        # index order in the firmware rows
PASS_ORDER = ["V", "v", "Q", "d", "A", "B", "W"]   # per-pass order in taskSoak

def reply_len(step, entries):
    """Approximate wire length of a module's reply, by the entries its map holds."""
    d = 10 + entries * 8
    return {"v": 32, "Q": 26, "V": 32, "d": d, "A": d + 95, "B": d + 95,
            "W": d}[step]

def simulate(sc, minutes=10, seed=1):
    rnd = random.Random(seed)
    ids = sc["ids"]
    faults = sc.get("faults", {})
    far = set(sc.get("far", []))
    term = sc.get("term", 0.0)                     # 0 = terminated properly
    rows = {i: {"id": i, **{k: [0, 0] for k in STEPS}, "max": 0, "streak": 0, "worst": 0,
                "alt": 0, "bad": 0, "dup": 0, "ww": 0} for i in ids}
    entries = {i: rnd.choice([0, 6, 20, 40, 64]) for i in ids}
    queries = failures = writes = drops = rejects = dups = stray = restore_fail = 0
    elapsed = 0.0
    n = len(ids)
    per_pass_s = {"v": 0.35 * n, "Q": 0.35 * n, "d": 0.9 * n, "A": 1.1 * n, "W": 3.5 * n,
                  "V": 0.03 + 0.1 * (max(ids) - min(ids) + 1) + 0.6,
                  "B": 0.03 + 0.7 * (max(ids) - min(ids) + 1) + 1.5}
    burst = {i: 0 for i in ids}                   # contact faults come in bursts

    def lost(i, step, length):
        f = faults.get(i, {})
        if "silent" in f: return True
        p = 0.0
        if term:                                  # long-frame loss grows with length^2
            p += term * (length / 768.0) ** 2 * (2.5 if i in far else 1.0)
        if "contact" in f:
            if burst[i] > 0: burst[i] -= 1; p += 0.85
            elif rnd.random() < f["contact"]: burst[i] = rnd.randint(1, 4); p += 0.85
        if "nodump" in f and step in ("d", "A", "B"): return True
        if "dup" in f and step in ("v", "Q", "d", "A", "W"):
            p += 0.5                              # two boards answer at once
        return rnd.random() < min(p, 0.98)

    def score(r, k, ok):
        nonlocal queries, failures
        r[k][1] += 1; queries += 1
        if ok:
            r[k][0] += 1; r["streak"] = 0
        else:
            failures += 1; r["streak"] += 1; r["worst"] = max(r["worst"], r["streak"])

    passes = 0
    while elapsed < minutes * 60:
        step = PASS_ORDER[passes % 7]
        elapsed += per_pass_s[step]
        if step in ("V", "B"):
            seen = {}
            for i in ids:
                f = faults.get(i, {})
                length = reply_len(step, entries[i])
                if step == "B" and "old" in f: continue
                copies = 2 if "dup" in f else 1
                got = 0; bad = 0
                for _ in range(copies):
                    if not lost(i, step, length):
                        got += 1
                if got == 2:
                    # two replies in one slot: they collide; often one is damaged, both arrive
                    if rnd.random() < 0.7: bad += 1; rejects += 1
                if got == 0 and term and length > 300 and rnd.random() < 0.5: drops += 1
                seen[i] = (got, bad)
            for extra in sc.get("stray", []):
                if rnd.random() < 0.9: stray += 1
            for i in ids:
                r = rows[i]
                if step == "B" and "old" in faults.get(i, {}): continue
                got, bad = seen[i]
                r["bad"] += bad
                if got > 1: r["dup"] += 1; dups += got - 1
                score(r, step, got == 1 and bad == 0)
        else:
            for i in ids:
                r = rows[i]; f = faults.get(i, {})
                if step in ("Q",) and "old" in f: continue
                if step in ("A",) and "old" in f: continue
                length = reply_len(step, entries[i])
                if step == "W":
                    if lost(i, "d", reply_len("d", entries[i])): score(r, "W", False); continue
                    writes += 1
                    ok = True
                    if "eeprom" in f and rnd.random() < f["eeprom"]: ok = False; r["ww"] |= 1
                    if lost(i, "d", reply_len("d", entries[i])): ok = False; r["ww"] |= 1
                    score(r, "W", ok)
                else:
                    ok = not lost(i, step, length)
                    if ok: r["max"] = max(r["max"], length)
                    elif term and length > 300 and rnd.random() < 0.4: drops += 1
                    score(r, step, ok)
        passes += 1
    for r in rows.values():
        r["streak"] = 0 if r["streak"] == 0 else r["streak"]
    return {"ok": True, "state": "done", "modules": n, "passes": passes, "queries": queries,
            "failures": failures, "elapsed": int(elapsed * 1000), "remaining": 0,
            "drops": drops, "rejects": rejects, "writes": writes, "restoreFail": restore_fail,
            "dups": dups, "stray": stray, "rows": [rows[i] for i in ids]}

IDS = list(range(15))
SCENARIOS = {
    "healthy":      {"ids": IDS},
    "termination":  {"ids": IDS, "term": 0.55, "far": [11, 12, 13, 14]},
    "contact":      {"ids": IDS, "faults": {7: {"contact": 0.12}, 8: {"contact": 0.04}}},
    "duplicate-id": {"ids": IDS, "faults": {4: {"dup": True}}},
    "eeprom":       {"ids": IDS, "faults": {3: {"eeprom": 0.9}}},
    "silent":       {"ids": IDS, "faults": {12: {"silent": True}}, "stray": [40]},
    "no-dump":      {"ids": IDS, "faults": {10: {"nodump": True}, 11: {"nodump": True}}},
    "mixed":        {"ids": IDS, "term": 0.25, "far": [13, 14], "faults": {2: {"contact": 0.08}, 9: {"old": True}}},
}

if __name__ == "__main__":
    name = sys.argv[1] if len(sys.argv) > 1 else "healthy"
    minutes = int(sys.argv[2]) if len(sys.argv) > 2 else 10
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    if name == "all":
        out = os.environ.get("OUT", ".")
        for k, sc in SCENARIOS.items():
            with open(os.path.join(out, "soak-%s.json" % k), "w") as fh:
                json.dump(simulate(sc, minutes, seed), fh)
            print("wrote", k)
    else:
        print(json.dumps(simulate(SCENARIOS[name], minutes, seed)))
