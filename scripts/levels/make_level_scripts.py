"""mechassault-recomp: writes scripts/levels/pad_<LEVEL>.txt, one pad script per campaign level: with
unlock_campaign = true, Select Mission lists the whole campaign (src/game/campaign.cpp), the ending at the bottom, so
a level is that many "up"s from it; then briefing, 'Mech select, launch, and 45 s of walking, turning, firing and jump
jets. python scripts/levels/make_level_scripts.py"""
import os
nodes = ["BEGINMOVIE", "DROPSHIPCRASH", "LEVEL01", "GRASSLANDMOVIE", "LEVEL02", "LEVEL03", "LEVEL04", "LEVEL05",
         "WASTELANDMOVIE", "LEVEL06A", "LEVEL06B", "LEVEL07A", "LEVEL07B", "LEVEL09", "LEVEL10", "LEVEL12", "LEVEL13",
         "LEVEL15A", "LEVEL15B", "LEVEL17", "LEVEL18", "LEVEL19", "LEVEL20A", "LEVEL20B"]
here = os.path.dirname(os.path.abspath(__file__))
for i, n in enumerate(nodes):
    if not n.startswith("LEVEL"):
        continue
    ups = len(nodes) - i
    lines = [f"# {n}: Select Mission, {ups} up from the ending, then briefing, 'Mech select, launch, play",
             "20 start 200", "24 a 200", "28 a 200"]  # the logo movies run at their own speed now: the title is up by ~18 s
    t = 32.0
    for _ in range(ups):
        lines.append(f"{t:.1f} up 150")
        t += 0.5
    s = t + 1.5
    lines += [f"{s:.1f} a 200", f"{s + 6:.1f} a 200", f"{s + 10:.1f} start 200", f"{s + 14:.1f} a 200",
              f"{s + 21:.1f} a 200", f"{s + 28:.1f} a 200", f"{s + 34:.1f} a 200"]
    p = s + 50
    lines += [f"{p:.1f} ly 6000 30000", f"{p + 6:.1f} rx 2000 20000", f"{p + 9:.1f} rt 1500", f"{p + 11:.1f} ly 5000 30000",
              f"{p + 16:.1f} l3 1500", f"{p + 18:.1f} rx 1500 -20000", f"{p + 20:.1f} rt 2000", f"{p + 23:.1f} y 200",
              f"{p + 24:.1f} rt 1500", f"{p + 26:.1f} ly 8000 30000", f"{p + 34:.1f} x 200", f"{p + 35:.1f} rt 2000",
              f"{p + 40:.1f} ly 4000 -30000"]
    open(os.path.join(here, f"pad_{n}.txt"), "w").write("\n".join(lines) + "\n")
    print(n, ups, f"{p + 46:.0f} s")
