#!/usr/bin/env python3
"""Skyrift PAPP:  python game/skyrift/build.py -> build/skyrift/skyrift.papp"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(HERE)), "examples", "papp"))
import papp_common  # noqa: E402

if __name__ == "__main__":
    print(papp_common.build("skyrift", HERE, ["main.c"]))
