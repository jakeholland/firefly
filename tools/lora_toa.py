#!/usr/bin/env python3
"""LoRa time-on-air, Semtech SX1261/2 datasheet formula (§6.1.4).

Used by docs/specs/S29-radio-only.md's airtime arithmetic. Run it rather than
trusting a remembered number — the figure this spec originally carried
(150-180 ms for a ~22 B packet at SF11/BW250) was wrong by roughly 2x, which
matters because the FIND ping cadence is chosen against it.

    python3 tools/lora_toa.py
"""
import math


def time_on_air_ms(payload_bytes, sf=11, bw_hz=250e3, cr=1, preamble=8,
                   crc=True, implicit_header=False, low_data_rate=None):
    """Milliseconds on air for one LoRa packet.

    `cr` is the coding-rate index: 1 => 4/5 ... 4 => 4/8.
    `low_data_rate` defaults to the datasheet's own rule (enable when the
    symbol time exceeds 16 ms); SF11/BW250 gives 8.192 ms, so it is off.
    """
    t_sym = (2 ** sf) / bw_hz
    if low_data_rate is None:
        low_data_rate = t_sym > 0.016
    de = 1 if low_data_rate else 0
    t_preamble = (preamble + 4.25) * t_sym
    numerator = 8 * payload_bytes - 4 * sf + 28 + (16 if crc else 0) - (20 if implicit_header else 0)
    denominator = 4 * (sf - 2 * de)
    payload_symbols = 8 + max(math.ceil(numerator / denominator) * (cr + 4), 0)
    return (t_preamble + payload_symbols * t_sym) * 1000.0, payload_symbols


if __name__ == "__main__":
    # S29 FIND: PING is 6 B of app payload, PONG 11 B, plus ~16 B of
    # Meshtastic/RadioHead framing for a direct encrypted packet.
    ping_ms, ping_n = time_on_air_ms(22)
    pong_ms, pong_n = time_on_air_ms(27)
    print("LONG_FAST = SF11 / BW250 kHz / CR 4-5, 8-symbol preamble, CRC on")
    print(f"  symbol time      {(2 ** 11) / 250e3 * 1000:.3f} ms (< 16 ms, so low-data-rate optimisation is OFF)")
    print(f"  PING  (22 B)     {ping_n} payload symbols  ->  {ping_ms:.0f} ms")
    print(f"  PONG  (27 B)     {pong_n} payload symbols  ->  {pong_ms:.0f} ms")
    round_trip = ping_ms + pong_ms
    print(f"  round trip                                  {round_trip:.0f} ms")
    for interval_ms, label in ((10000, "10 s (pre-2026-09-16)"), (5000, "5 s (current)")):
        one = round_trip / interval_ms * 100
        print(f"  at {label:<22} {one:.1f}% of channel for one session, {2 * one:.1f}% for two")
