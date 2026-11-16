#!/usr/bin/env python3
"""Summarize a private NMEA capture without printing location coordinates."""
import argparse
import json
from pathlib import Path


def summarize(data):
    result = dict(sentences=0, checksum_errors=0, malformed=0, gga=0,
                  gga_valid=0, rmc=0, rmc_valid=0, satellites_used_max=0)
    for raw in data.splitlines():
        if not raw.startswith(b"$"):
            continue
        result["sentences"] += 1
        try:
            body, checksum = raw[1:].rsplit(b"*", 1)
            if len(checksum) != 2:
                raise ValueError("checksum length")
            actual = 0
            for byte in body:
                actual ^= byte
            if actual != int(checksum, 16):
                result["checksum_errors"] += 1
                continue
            fields = body.decode("ascii").split(",")
            if fields[0] in ("GPGGA", "GNGGA"):
                result["gga"] += 1
                quality, satellites = int(fields[6]), int(fields[7])
                if not 0 <= quality <= 8 or not 0 <= satellites <= 99:
                    raise ValueError("GGA bounds")
                result["gga_valid"] += quality != 0
                result["satellites_used_max"] = max(result["satellites_used_max"], satellites)
            elif fields[0] in ("GPRMC", "GNRMC"):
                result["rmc"] += 1
                if fields[2] not in ("A", "V"):
                    raise ValueError("RMC status")
                result["rmc_valid"] += fields[2] == "A"
        except (ValueError, IndexError, UnicodeError):
            result["malformed"] += 1
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    args = parser.parse_args()
    print(json.dumps(summarize(args.capture.read_bytes()), sort_keys=True))
