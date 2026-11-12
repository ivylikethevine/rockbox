"""Simulate the SBC media tick's packet output: packets per second and the
gaps between them, for the old 10 ms tick (part packets) and the per-packet
tick (full packets only), with wake-ups up to a given lateness.

Mirrors a2dp_sbc_get_num_frame_iteration() in apps/bluetooth/codecs/sbc.c at
44.1 kHz stereo, 16 blocks, 8 subbands (512 PCM bytes per frame).

Run: python3 scratch/sbc_pacing.py [frames_per_packet] [max_late_ms]
"""
import random
import sys

RATE: int = 44100
FRAME_BYTES: int = 512
FRAME_SAMPLES: int = 128
BYTES_PER_10MS: int = RATE * 4 * 10 // 1000


def run(period_us: float, frames_per_packet: int, full_only: bool,
        max_late_us: float, seconds: int = 60) -> tuple[float, float, float]:
    rng: random.Random = random.Random(1)
    counter: float = 0.0
    last: float | None = None
    t_next: float = 0
    sends: list[float] = []
    while t_next < seconds * 1_000_000:
        now: float = t_next + rng.uniform(0, max_late_us)
        tick_us: float = 10_000 if last is None else now - last
        last = now
        counter += BYTES_PER_10MS * tick_us / 10_000
        projected: int = int(counter / FRAME_BYTES)
        nof: int = frames_per_packet
        noi: int = 1
        if nof < projected:
            noi = min(projected // nof, 3)
        elif projected < nof and full_only:
            nof = 0
        else:
            nof = projected
        counter -= noi * nof * FRAME_BYTES
        if nof:
            sends.extend([now] * noi)
        t_next += period_us
    gaps: list[float] = [(b - a) / 1000 for a, b in zip(sends, sends[1:])]
    return len(sends) / seconds, min(gaps), max(gaps)


def main() -> None:
    fpp: int = int(sys.argv[1]) if len(sys.argv) > 1 else 5
    late_us: float = float(sys.argv[2]) * 1000 if len(sys.argv) > 2 else 2000
    packet_us: float = fpp * FRAME_SAMPLES * 1_000_000 / RATE
    print(f"{fpp} frames a packet, wake-ups up to {late_us / 1000:.1f} ms late")
    print(f"{'pacing':<22}{'packets/s':>10}{'gap min ms':>12}{'gap max ms':>12}")
    for name, period, full in (("10 ms tick, part", 10_000, False),
                               ("10 ms tick, full", 10_000, True),
                               ("per packet, full", packet_us, True)):
        rate, gmin, gmax = run(period, fpp, full, late_us)
        print(f"{name:<22}{rate:>10.1f}{gmin:>12.1f}{gmax:>12.1f}")


if __name__ == "__main__":
    main()
