#!/usr/bin/env python3
"""Host-side control and I-V plotting for the Mini SMU.

Examples:
  python smu.py --port /dev/ttyACM0 sweep --start 0 --stop 3000 --step 20 --csv led.csv --plot led.png
  python smu.py --simulate sweep --start 0 --stop 3000 --step 20 --plot sim.png
  python smu.py --port COM5 measure
"""
import argparse
import csv
import math
import random
import sys

import numpy as np

SHUNT_OHMS = 10.0
COMPLIANCE_UA = 7500.0


class SerialSMU:
    """Talks to the real board over USB CDC."""

    def __init__(self, port, baud=115200, timeout=3.0):
        import serial
        self.ser = serial.Serial(port, baud, timeout=timeout)

    def _cmd(self, text):
        self.ser.write((text + "\n").encode())
        return self.ser.readline().decode().strip()

    def identify(self):
        return self._cmd("ID?")

    def set_mv(self, mv):
        return self._cmd(f"SET {mv}")

    def off(self):
        return self._cmd("OFF")

    def measure(self):
        reply = self._cmd("MEAS?")
        if reply.startswith("ERR"):
            raise RuntimeError(reply)
        v, i = reply.split(",")
        return float(v), float(i)

    def sweep(self, start, stop, step, settle_ms):
        self.ser.timeout = max(5.0, settle_ms / 1000 * 2)
        self.ser.write(f"SWEEP {start} {stop} {step} {settle_ms}\n".encode())
        rows = []
        while True:
            line = self.ser.readline().decode().strip()
            if not line:
                raise TimeoutError("No response from board during sweep")
            if line.startswith("BEGIN"):
                continue
            if line == "END":
                break
            if line.startswith("ERR"):
                print(f"[board] {line}", file=sys.stderr)
                if "COMPLIANCE" in line:
                    continue
                raise RuntimeError(line)
            v_set, v_dut, i_ua = line.split(",")
            rows.append((float(v_set), float(v_dut), float(i_ua)))
        return rows


class SimulatedSMU:
    """Software model of a red LED + 10 ohm shunt, for testing without hardware.

    Results from this class are SIMULATED and must never be presented as
    measurements from the physical board.
    """

    IS, N, VT, RS = 1e-19, 1.9, 0.02585, 5.0

    def __init__(self, noise_ua=0.3):
        self.noise_ua = noise_ua
        self.v_set = 0.0

    def identify(self):
        return "MINI-SMU-SIM"

    def set_mv(self, mv):
        self.v_set = mv
        return "OK"

    def off(self):
        self.v_set = 0.0
        return "OK"

    def _solve_current(self, v_set_v):
        lo, hi = 0.0, 0.1
        for _ in range(80):
            mid = (lo + hi) / 2
            vd = self.N * self.VT * math.log(mid / self.IS + 1.0) + mid * self.RS
            if mid * SHUNT_OHMS + vd > v_set_v:
                hi = mid
            else:
                lo = mid
        return lo

    def measure(self):
        i_a = self._solve_current(self.v_set / 1000.0)
        i_ua = i_a * 1e6 + random.gauss(0, self.noise_ua)
        v_dut_mv = self.v_set - i_a * SHUNT_OHMS * 1000.0
        return v_dut_mv, i_ua

    def sweep(self, start, stop, step, settle_ms):
        if (stop - start) * step < 0:
            step = -step
        rows = []
        for k in range((stop - start) // step + 1):
            v_set = start + k * step
            self.set_mv(v_set)
            v, i = self.measure()
            if abs(i) > COMPLIANCE_UA:
                print(f"[sim] compliance at {v_set} mV", file=sys.stderr)
                break
            rows.append((float(v_set), v, i))
        self.off()
        return rows


def save_csv(rows, path):
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["v_set_mV", "v_dut_mV", "i_uA"])
        w.writerows(rows)


def plot_iv(rows, path, title, simulated=False):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    data = np.array(rows)
    v = data[:, 1] / 1000.0
    i_ma = data[:, 2] / 1000.0

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(10, 4.2))
    ax1.plot(v, i_ma, "o-", ms=3, lw=1.2, color="#1f6feb")
    ax1.set_xlabel("DUT voltage (V)")
    ax1.set_ylabel("Current (mA)")
    ax1.set_title("Linear")
    ax1.grid(alpha=0.3)

    mask = i_ma > 1e-4
    ax2.semilogy(v[mask], i_ma[mask], "o-", ms=3, lw=1.2, color="#d1242f")
    ax2.set_xlabel("DUT voltage (V)")
    ax2.set_ylabel("Current (mA, log)")
    ax2.set_title("Semi-log")
    ax2.grid(alpha=0.3, which="both")

    label = f"{title} (SIMULATED DATA)" if simulated else title
    fig.suptitle(label, fontweight="bold")
    fig.tight_layout()
    fig.savefig(path, dpi=150)
    print(f"Saved plot: {path}")


def main():
    p = argparse.ArgumentParser(description="Mini SMU host tool")
    p.add_argument("--port", help="Serial port, e.g. /dev/ttyACM0 or COM5")
    p.add_argument("--simulate", action="store_true", help="Use built-in LED model, no hardware")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("id")
    sub.add_parser("measure")
    s = sub.add_parser("sweep")
    s.add_argument("--start", type=int, default=0, help="mV")
    s.add_argument("--stop", type=int, default=3000, help="mV")
    s.add_argument("--step", type=int, default=20, help="mV")
    s.add_argument("--settle", type=int, default=20, help="settle time per point, ms")
    s.add_argument("--csv", help="write CSV here")
    s.add_argument("--plot", help="write PNG here")
    s.add_argument("--title", default="I-V characteristic")
    args = p.parse_args()

    if args.simulate:
        smu = SimulatedSMU()
    elif args.port:
        smu = SerialSMU(args.port)
    else:
        p.error("give --port or --simulate")

    if args.cmd == "id":
        print(smu.identify())
    elif args.cmd == "measure":
        v, i = smu.measure()
        print(f"V = {v:.2f} mV, I = {i:.3f} uA")
    elif args.cmd == "sweep":
        rows = smu.sweep(args.start, args.stop, args.step, args.settle)
        print(f"Collected {len(rows)} points")
        if args.csv:
            save_csv(rows, args.csv)
            print(f"Saved CSV: {args.csv}")
        if args.plot:
            plot_iv(rows, args.plot, args.title, simulated=args.simulate)


if __name__ == "__main__":
    main()
