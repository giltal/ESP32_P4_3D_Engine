#!/usr/bin/env python3
"""shadows PAPP:  python examples/papp/shadows/build.py -> build/e3d_shadows/"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import papp_common  # noqa: E402

if __name__ == "__main__":
    papp_common.build("e3d_shadows", HERE, ["main.c"])
