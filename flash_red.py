#!/usr/bin/env python3
"""Flash Hue lights red, then leave them red. Use --reset to go back to neutral white."""
import argparse
import json
import time
from pathlib import Path

import requests

LIGHTS = ["43"]  # Treehouse color
FLASHES = 5
INTERVAL = 0.5  # seconds per on/off phase

RED = {"on": True, "bri": 254, "xy": [0.675, 0.322], "transitiontime": 0}
DARK = {"bri": 1, "transitiontime": 0}
WHITE = {"on": True, "bri": 254, "ct": 366} 

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--reset", action="store_true", help="set lights to neutral white instead of flashing")
args = parser.parse_args()

config = json.loads((Path(__file__).parent / "config.json").read_text())
base = f"http://{config['bridge']}/api/{config['username']}/lights"


def set_state(state):
    for light in LIGHTS:
        requests.put(f"{base}/{light}/state", json=state, timeout=5).raise_for_status()


if args.reset:
    set_state(WHITE)
else:
    for _ in range(FLASHES):
        set_state(RED)
        time.sleep(INTERVAL)
        set_state(DARK)
        time.sleep(INTERVAL)

    set_state(RED)
