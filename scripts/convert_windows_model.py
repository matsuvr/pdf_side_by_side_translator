#!/usr/bin/env python3
"""Build-time-only conversion of the exact pinned PFN BF16 model into P4X86 v7."""
import argparse
from pathlib import Path
import sys
from common import ROOT, lock, run, sha256

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source-dir',type=Path,required=True)
    ap.add_argument('--output',type=Path,required=True)
    a=ap.parse_args()
    spec=lock()['windows']
    src=a.source_dir.resolve()
    dst=a.output.resolve()
    if dst.exists():
        raise SystemExit('Output already exists; choose a new path.')
    for name,digest in spec['source_sha256'].items():
        p=src/name
        if not p.is_file() or sha256(p)!=digest:
            raise SystemExit('Missing or mismatched pinned source: '+name)
    crate=ROOT/'crates/plamo'
    run(['cargo','+1.100.0','run','--release','--locked','--offline','--example','convert_x86','--',src,dst],cwd=crate)
    if dst.stat().st_size!=spec['bytes'] or sha256(dst)!=spec['sha256']:
        raise SystemExit('Converted file differs from the pinned v7 bytes. Do not package it.')
    print('Verified PLaMo x86 S4 v7:',dst)
if __name__=='__main__':
    main()
