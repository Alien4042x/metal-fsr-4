#!/usr/bin/env python3
"""Run local builders while keeping private test inputs outside the project root."""
from pathlib import Path
import os, subprocess, sys
root=Path(__file__).resolve().parents[1]
private=root/'.local/workbench'
allowed={'build_runtime','build_fg_cube','run_fg_cube','run_game','build_game_fg','run_central_runtime_probe','sdk_contract'}
if len(sys.argv)<2 or sys.argv[1] not in allowed:
    raise SystemExit('Unknown local command')
env=os.environ.copy()
env['PYTHONPATH']=str(root/'.local/compat')
raise SystemExit(subprocess.run([sys.executable,str(private/'tools'/(sys.argv[1]+'.py')),*sys.argv[2:]],cwd=private,env=env).returncode)
