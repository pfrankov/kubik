#!/usr/bin/env python3
"""Render the real Skies catalog and five telescope-focus stages with the native firmware renderer."""
import argparse
import os
from pathlib import Path
import runpy
import subprocess
import tempfile
import zlib
import struct

ROOT=Path(__file__).resolve().parent.parent


def png(rgb):
    def chunk(kind,data):
        return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
    rows=b''.join(b'\0'+rgb[y*1440:(y+1)*1440] for y in range(480))
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',480,480,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(rows))+chunk(b'IEND',b'')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,default=ROOT/'dist/skies-preview')
    args=parser.parse_args();args.out.mkdir(parents=True,exist_ok=True)
    sources=runpy.run_path(str(ROOT/'tools/test-frames.py'))['SOURCES']
    with tempfile.TemporaryDirectory(prefix='kubik-skies-preview-') as tmp:
        exe=Path(tmp)/'preview'
        subprocess.run([os.environ.get('CC','cc'),'-O2','-DKUBIK_SIM_EMBEDDED','-Ifirmware/main','-Ifirmware/managed_components/espressif__qrcode',
                        'firmware/sim/sky_preview.c',*[f'firmware/{s}.c' for s in sources],'-lm','-o',str(exe)],cwd=ROOT,check=True)
        frames=subprocess.check_output([str(exe)],cwd=ROOT)
    names=['triangulum','crux','corvus','delphinus','cassiopeia','cepheus','ursa-minor','orion',*[f'focus-{i}' for i in range(5)],*[f'orion-focus-{i}' for i in range(5)],*[f'stream-{i}' for i in range(3)]]
    size=480*480*3;assert len(frames)==len(names)*size
    for i,name in enumerate(names):(args.out/f'{name}.png').write_bytes(png(frames[i*size:(i+1)*size]))
    print(f'{len(names)} production-rendered frames: {args.out}')


if __name__=='__main__':main()
