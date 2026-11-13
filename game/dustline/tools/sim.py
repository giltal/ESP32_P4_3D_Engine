"""Host sim of Dustline's car + autopilot, a copy of main.c's model in Python, for tuning
without the board. It reads the course and the CAR_* constants from main.c; the other
parameters below mirror main.c by hand (keep them in step). Override any: KEY=value.

    python game/dustline/tools/sim.py                 # one lap by the autopilot
    python game/dustline/tools/sim.py RG=1.1 DF=0.35  # try other values
"""
import math, os, re, sys

SRC = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "main.c"), encoding="utf-8").read()

def cdef(name):
    m = re.search(r"#define\s+" + name + r"\s+\(?([-0-9.]+)f?", SRC)
    return float(m.group(1))

P = {k: cdef(k) for k in ["CAR_M", "CAR_I", "CAR_A", "CAR_B", "CAR_CF", "CAR_CR", "CAR_POWER", "CAR_FMAX",
                          "CAR_DRAG", "CAR_BRAKE", "CAR_STEER"]}
# as in main.c: SAT_* = side force kept past the peak, RG = rear grip, DF = front drive share,
# SL = lock lost with speed, SR = wheel turn rate in full locks/s (0 = 2.6 rad/s), RGL = rear grip on the loose surfaces, TC / ABS = least share of grip left to drive / brakes, WT = weight transfer, BB = front brake share
P.update(dict(SAT_F=1.0, SAT_R=1.0, AUTO_MU=0.6, AUTO_K=2.6, AUTO_SLIDE=1.2, HAND=0.45, WT=0.55, BB=0.6, YD=0.0,
              ABS=0.5, HOR=45, RG=1.15, RGL=1.15, DF=0.4, TC=0.3, LOOK=0.4, YAWK=0.08, SL=0.045, SR=0.0))
for a in sys.argv[1:]:
    k, v = a.split("=")
    P[k] = float(v)

SURF = {"SURF_TARMAC": 0, "SURF_DIRT": 1, "SURF_GRAVEL": 2}
ctrl = [(float(x), float(z), SURF[s]) for x, z, s in
        re.findall(r"\{\s*([-0-9.]+),\s*([-0-9.]+),\s*(SURF_\w+)\s*\}", SRC.split("k_ctrl[] = {")[1].split("};")[0])]
MU = [1.05, 0.80, 0.70, 0.62, 0.50]
DRAGS = [0, 30, 50, 220, 420]
G = 9.81
STEP = 2.0

def catmull(p0, p1, p2, p3, t):
    t2, t3 = t * t, t * t * t
    f = lambda a, b, c, d: 0.5 * (2 * b + (-a + c) * t + (2 * a - 5 * b + 4 * c - d) * t2 + (-a + 3 * b - 3 * c + d) * t3)
    return f(p0[0], p1[0], p2[0], p3[0]), f(p0[1], p1[1], p2[1], p3[1])

def build():
    n = len(ctrl); pts = []
    for i in range(n):
        p0, p1, p2, p3 = ctrl[(i - 1) % n], ctrl[i], ctrl[(i + 1) % n], ctrl[(i + 2) % n]
        for s in range(64):
            x, z = catmull(p0, p1, p2, p3, s / 64)
            pts.append((x, z, p1[2]))
    pts.append(pts[0])
    seglen = [math.hypot(pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1]) for i in range(len(pts) - 1)]
    total = sum(seglen); ns = int(total / STEP); step = total / ns
    tr = []; seg = 0; ss = 0
    for k in range(ns):
        d = k * step
        while d > ss + seglen[seg] and seg < len(seglen) - 1:
            ss += seglen[seg]; seg += 1
        t = (d - ss) / seglen[seg] if seglen[seg] else 0
        a, b = pts[seg], pts[seg + 1]
        tr.append(dict(x=a[0] + (b[0] - a[0]) * t, z=a[1] + (b[1] - a[1]) * t, surf=a[2], hw=4.8 if a[2] == 0 else 4.3))
    for k in range(ns):
        a, b = tr[k - 1], tr[(k + 1) % ns]
        l = math.hypot(b["x"] - a["x"], b["z"] - a["z"])
        tr[k]["tx"], tr[k]["tz"] = (b["x"] - a["x"]) / l, (b["z"] - a["z"]) / l
    for k in range(ns):
        a, b = tr[k - 2], tr[(k + 2) % ns]
        dh = (math.atan2(b["tz"], b["tx"]) - math.atan2(a["tz"], a["tx"]) + 3 * math.pi) % (2 * math.pi) - math.pi
        tr[k]["curv"] = dh / (4 * STEP)
    return tr, total

TR, TOTAL = build()
NS = len(TR)

def nearest(x, z, hint, span=6):
    best, bd = hint, 1e18
    for o in range(-span, span + 1):
        k = (hint + o) % NS
        d = (TR[k]["x"] - x) ** 2 + (TR[k]["z"] - z) ** 2
        if d < bd: bd, best = d, k
    return best

def lateral(k, x, z):
    s = TR[k]
    return (x - s["x"]) * -s["tz"] + (z - s["z"]) * s["tx"]

class Car: pass

def surface(c):
    s = TR[c.idx]; d = abs(lateral(c.idx, c.x, c.z))
    return s["surf"] if d <= s["hw"] else 3 if d <= s["hw"] + 1.4 else 4

def clamp(v, a, b): return a if v < a else b if v > b else v

def step(c, steer, gas, brake, hand, dt):
    ch, sh = math.cos(c.h), math.sin(c.h)
    vf = c.vx * ch + c.vz * sh; vl = -c.vx * sh + c.vz * ch
    mu = MU[c.surf]; speed = math.hypot(vf, vl)
    lock = P["CAR_STEER"] / (1 + speed * P["SL"])
    want = steer * lock; rate = (P["SR"] * lock if P["SR"] > 0 else 2.6) * dt
    c.steer += clamp(want - c.steer, -rate, rate); d = c.steer
    A, Bb, M = P["CAR_A"], P["CAR_B"], P["CAR_M"]
    nf = M * G * Bb / (A + Bb) - c.ax_s * M * 0.5 / (A + Bb) * P["WT"]
    nr = M * G - nf; nf = max(nf, 1500); nr = max(nr, 1500)
    drive = 0
    if gas > 0: drive = gas * min(P["CAR_FMAX"], P["CAR_POWER"] / max(abs(vf), 3))
    br = 0
    if brake > 0:
        if vf > 1: br = brake * P["CAR_BRAKE"]
        else: drive -= brake * 5200
    if vf < -12 and drive < 0: drive = 0
    fxf, fxr = drive * P["DF"], drive * (1 - P["DF"])
    bs = 1 if vf > 0 else -1
    fxf -= br * P["BB"] * bs; fxr -= br * (1 - P["BB"]) * bs
    if hand: fxr -= 4500 * bs * min(1, abs(vf))
    vla = vl + A * c.r
    wl = vla * math.cos(d) - vf * math.sin(d); wf = vf * math.cos(d) + vla * math.sin(d)
    af = math.atan2(wl, max(abs(wf), 1.5)); ar = math.atan2(vl - Bb * c.r, max(abs(vf), 1.5))
    capf = mu * nf; capr = mu * nr * (P["RG"] if c.surf == 0 else P["RGL"]) * (P["HAND"] if hand else 1)
    pf = getattr(c, "pf", 0.0); pr = getattr(c, "pr", 0.0)
    def lim(fx, cap, py, hb=False):
        if hb: return clamp(fx, -cap, cap)
        room = math.sqrt(max(0, cap * cap - py * py))
        if fx > 0: return min(fx, max(room, P["TC"] * cap))
        return max(fx, -max(room, P["ABS"] * cap))
    fxf = lim(fxf, capf, pf); fxr = lim(fxr, capr, pr, hand)
    limf = math.sqrt(max(0, capf * capf - fxf * fxf)); limr = math.sqrt(max(0, capr * capr - fxr * fxr))
    fyf = -P["CAR_CF"] * af; fyr = -P["CAR_CR"] * ar
    if abs(fyf) > limf: fyf = math.copysign(limf * P["SAT_F"], fyf)
    if abs(fyr) > limr: fyr = math.copysign(limr * P["SAT_R"], fyr)
    c.dbg = (fyf/limf if limf else 9, fyr/limr if limr else 9, af, ar, nf, nr)
    c.pf, c.pr = fyf, fyr
    fx = fxf * math.cos(d) - fyf * math.sin(d) + fxr
    fy = fxf * math.sin(d) + fyf * math.cos(d) + fyr
    drag = P["CAR_DRAG"] * speed + DRAGS[c.surf]
    fx -= drag * vf; fy -= drag * vl
    tq = A * (fxf * math.sin(d) + fyf * math.cos(d)) - Bb * fyr - P["YD"] * c.r
    ax, ay = fx / M, fy / M
    vf += (ax + vl * c.r) * dt; vl += (ay - vf * c.r) * dt
    c.r += tq / P["CAR_I"] * dt
    if speed < 2:
        k = math.exp(-dt * 6); vl *= k
        if abs(vf) < 1 and gas <= 0 and brake <= 0: vf *= k; c.r *= k
    c.h += c.r * dt
    c.h = (c.h + math.pi) % (2 * math.pi) - math.pi
    ch, sh = math.cos(c.h), math.sin(c.h)
    c.vx = vf * ch - vl * sh; c.vz = vf * sh + vl * ch
    c.x += c.vx * dt; c.z += c.vz * dt
    c.vf, c.vl = vf, vl
    c.ax_s += (ax - c.ax_s) * min(1, dt * 5)

def auto(c):
    speed = math.hypot(c.vx, c.vz)
    look = 3 + int(speed * P["LOOK"])
    k = (c.idx + look) % NS
    dx, dz = TR[k]["x"] - c.x, TR[k]["z"] - c.z
    ld = max(math.hypot(dx, dz), 4)
    want = math.atan2(dz, dx)
    err = (want - c.h + 3 * math.pi) % (2 * math.pi) - math.pi
    kap = 2 * math.sin(err) / ld
    lock = P["CAR_STEER"] / (1 + speed * P["SL"])
    delta = math.atan(kap * 2.5) * (1 + speed * 0.01)
    rdes = kap * max(speed, 1)
    delta += P["YAWK"] * (rdes - c.r)
    steer = clamp(delta / lock, -1, 1)
    vmax = 60
    for i in range(0, int(P["HOR"]), 2):
        q = (c.idx + i) % NS
        cv = abs(TR[q]["curv"]) + 1e-4
        mu = MU[TR[q]["surf"]] * P["AUTO_MU"]
        v = math.sqrt(mu * G / cv)
        vb = math.sqrt(v * v + 2 * mu * G * 0.6 * i * STEP)
        vmax = min(vmax, vb)
    gas = 1 if speed < vmax - 1 else 0.4 if speed < vmax else 0
    brake = 1 if speed > vmax + 2 else 0
    return steer, gas, brake

def run(seconds=200, verbose=True):
    c = Car(); s = TR[0]
    c.x, c.z = s["x"] - s["tx"] * 9, s["z"] - s["tz"] * 9
    c.h = math.atan2(s["tz"], s["tx"]); c.vx = c.vz = c.r = c.steer = c.vf = c.vl = c.ax_s = 0
    c.idx = 0; c.surf = 0
    dt = 1 / 240; t = 0; prev = 0; laps = 0; offs = 0; was_off = False; maxoff = 0; vmaxs = 0
    lap_t = None
    fr = 0
    while t < seconds:
        if fr % 4 == 0:
            ctl = auto(c)
        fr += 1
        c.idx = nearest(c.x, c.z, c.idx); c.surf = surface(c)
        step(c, *ctl, False, dt)
        t += dt
        off = abs(lateral(c.idx, c.x, c.z)) - TR[c.idx]["hw"]
        maxoff = max(maxoff, off)
        if (off > 0.5) != was_off:
            was_off = off > 0.5
            if was_off:
                offs += 1
                if verbose: print(f"  off at {c.idx} t={t:.1f} {math.hypot(c.vx,c.vz)*3.6:.0f} km/h curv {TR[c.idx]['curv']:.3f}")
        if off > 18:
            print("  LOST at", c.idx); return None
        if prev > NS - 40 and c.idx < 40 and t > 10:
            laps += 1
            if lap_t is None: lap_t = t
            if laps >= 1: break
        prev = c.idx
    print(f"lap {lap_t}, offs {offs}, max off {maxoff:.1f}, track {TOTAL:.0f} m")
    return lap_t

if __name__ == "__main__":
    run()
