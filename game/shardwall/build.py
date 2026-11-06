#!/usr/bin/env python3
"""Shardwall PAPP:  python game/shardwall/build.py -> build/shardwall/shardwall.papp"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(HERE)), "examples", "papp"))
import papp_common  # noqa: E402

if __name__ == "__main__":
    print(papp_common.build("shardwall", HERE, ["main.c"]))
