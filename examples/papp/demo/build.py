#!/usr/bin/env python3
"""The e3d demo show, as a PAPP.

    python examples/papp/demo/build.py      -> build/e3d_demo/e3d_demo.papp

The object files from ReferenceCode/ are embedded as C strings.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import papp_common  # noqa: E402

ASSETS = ["letters3d", "numbers3d", "marvell3d", "motorola3d"]

if __name__ == "__main__":
    ref = os.path.join(papp_common.ROOT, "ReferenceCode")
    papp_common.build("e3d_demo", HERE, ["main.c"],
                      embed={f"asset_{a}": os.path.join(ref, a + ".txt") for a in ASSETS})
