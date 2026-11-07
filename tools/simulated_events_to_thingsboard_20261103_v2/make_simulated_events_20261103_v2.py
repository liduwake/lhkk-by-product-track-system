"""make_simulated_events_20261103_v2.py - builds simulated_events_20261103_v2.csv
Purpose : SIMULATED example events (estimates, not measurements) for MP-01..MP-03,
          continuing v1 (7.9.-2.10.2026) for 3.10.-2.11.2026. Only the NEW events are written.
Version : v2 (2026-11-03). Changes from v1: new period; content_after continues from the last
          v1 row of each point; time offset from Europe/Helsinki (summer time ends 25.10.);
          MP-04 left out (device not created, Free plan limit); autumn break ignored (author).
Rules   : same as v1 - MP-01 2-3 additions 8-16 kg/week, one removal 2-4 kg (reuse),
          emptying Monday 07:30 with a small residue; MP-02 new module: offcuts Tue/Thu in
          weeks 1-2, dismantling 28.10. (30 m MMJ 3x2,5 S = 4.7 kg), emptying 30.10.;
          MP-03 4 additions/day 4.8-7.2 kg, emptying Wed and Fri 15:30.
AI usage: written by Claude (AI); run and checked by the author. Secrets: none.
"""
import csv, random, datetime as dt
from zoneinfo import ZoneInfo
random.seed(20261103)
HEL = ZoneInfo("Europe/Helsinki")
START = {"MP-01": 34100, "MP-02": 0, "MP-03": 0}   # content_after of the last v1 row
days = []
d = dt.date(2026, 10, 3)
while d <= dt.date(2026, 11, 2):
    if d.weekday() < 5: days.append(d)
    d += dt.timedelta(days=1)
assert len(days) == 21
raw = []
def ev(mp, day, hm, etype, change, after=None):
    h, m = map(int, hm.split(":"))
    t = dt.datetime(day.year, day.month, day.day, h, m, tzinfo=HEL).isoformat()
    raw.append([mp, t, etype, int(round(change)), after])

# ---- MP-01 wood offcuts (weekly pattern of v1) ----------------------------------
mondays = [x for x in days if x.weekday() == 0]
for mon in mondays:
    wd = [x for x in days if mon <= x < mon + dt.timedelta(days=7)]
    ev("MP-01", mon, "07:30", "emptying", 0, random.randint(3, 9) * 100)   # residue
    add_days = random.sample(wd, min(3, len(wd)))
    rem_day = random.choice(wd)
    for x in sorted(wd):
        if x in add_days:
            ev("MP-01", x, f"{random.choice([9,10,13,14]):02d}:{random.choice(['05','20','40','50'])}",
               "addition", random.randint(80, 160) * 100)
        if x == rem_day and len(wd) > 1:
            ev("MP-01", x, "11:15", "removal", -random.randint(20, 40) * 100)

# ---- MP-02 cable offcuts (new module in October) --------------------------------
for x in days:
    if x <= dt.date(2026, 10, 16) and x.weekday() in (1, 3):
        ev("MP-02", x, "14:30", "addition", random.randint(15, 30) * 10)
    if x == dt.date(2026, 10, 28):
        ev("MP-02", x, "13:00", "addition", 4700)
    if x == dt.date(2026, 10, 30):
        ev("MP-02", x, "14:00", "emptying", 0, 0)

# ---- MP-03 metal chips ----------------------------------------------------------
for x in days:
    for hm in ("09:30", "11:30", "13:30", "14:45"):
        ev("MP-03", x, hm, "addition", random.uniform(4800, 7200))
    if x.weekday() in (2, 4):
        ev("MP-03", x, "15:30", "emptying", 0, 0)

# ---- running balance, same rules as v1 ------------------------------------------
raw.sort(key=lambda r: (r[0], r[1]))
content = dict(START); rows = []
for mp, t, e, ch, residue in raw:
    c0 = content[mp]
    if e == "emptying":
        ch = residue - c0
    elif e == "removal" and c0 + ch < 0:
        continue
    content[mp] = c0 + ch
    rows.append([mp, t, e, ch, content[mp]])
with open("simulated_events_20261103_v2.csv", "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["measuring_point_id", "measured_at", "event_type", "weight_change_g", "content_after_g"])
    w.writerows(rows)

# ---- summary and checks ---------------------------------------------------------
from collections import defaultdict
s = defaultdict(lambda: defaultdict(int)); n = defaultdict(int)
for mp, t, e, ch, a in rows:
    n[mp] += 1; s[mp][e] += ch
for mp in sorted(n):
    print(mp, n[mp], "events", {k: round(v / 1000, 1) for k, v in s[mp].items()}, "kg")
prev = dict(START)
for mp, t, e, ch, a in rows:
    assert a == prev[mp] + ch and a >= 0, (mp, t)
    prev[mp] = a
print("rows", len(rows), "| first", rows[0][1], "| offsets", sorted({r[1][-6:] for r in rows}))
