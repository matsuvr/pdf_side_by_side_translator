#!/usr/bin/env python3
"""Original two-page icon, drawn from geometry; stdlib-only PNG/ICO/ICNS."""
from pathlib import Path
import struct
import zlib

ROOT=Path(__file__).resolve().parents[1]/'resources'
def rounded(x,y,l,t,r,b,radius):
    if not(l<=x<=r and t<=y<=b):return False
    cx=min(max(x,l+radius),r-radius);cy=min(max(y,t+radius),b-radius)
    return (x-cx)**2+(y-cy)**2<=radius**2
def color(x,y):
    if not rounded(x,y,0.03,0.03,0.97,0.97,0.19):return (0,0,0,0)
    base=(31,63,97,255)
    # Offset white/teal pages and their original text strokes.
    if rounded(x,y,0.17,0.19,0.51,0.76,0.045):base=(247,249,249,255)
    if rounded(x,y,0.47,0.27,0.82,0.84,0.045):base=(89,201,185,255)
    for top in (0.31,0.41,0.51,0.61):
        if rounded(x,y,0.23,top,0.43,top+0.025,0.012):base=(72,96,119,255)
    for top in (0.40,0.50,0.60,0.70):
        if rounded(x,y,0.54,top,0.75,top+0.025,0.012):base=(25,77,82,255)
    return base
def png(size):
    rows=bytearray();samples=2 if size>=256 else 4
    for y in range(size):
        rows.append(0)
        for x in range(size):
            values=[color((x+(sx+.5)/samples)/size,(y+(sy+.5)/samples)/size) for sy in range(samples) for sx in range(samples)]
            alpha=sum(c[3] for c in values)
            rows.extend([round(sum(c[k]*c[3] for c in values)/alpha) if alpha else 0 for k in range(3)]+[round(alpha/len(values))])
    def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',size,size,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(rows,9))+chunk(b'IEND',b'')
def main():
    images={n:png(n) for n in (16,32,64,128,256,512,1024)}
    (ROOT/'app-icon.png').write_bytes(images[256])
    parts=[]
    for kind,size in [(b'icp4',16),(b'icp5',32),(b'icp6',64),(b'ic07',128),(b'ic08',256),(b'ic09',512),(b'ic10',1024)]:
        parts.append(kind+struct.pack('>I',8+len(images[size]))+images[size])
    data=b''.join(parts);(ROOT/'app-icon.icns').write_bytes(b'icns'+struct.pack('>I',8+len(data))+data)
    sizes=(16,32,48,64,128,256);images[48]=png(48);offset=6+16*len(sizes);entries=[];parts=[]
    for size in sizes:
        data=images[size];entries.append(struct.pack('<BBBBHHII',size%256,size%256,0,0,1,32,len(data),offset));parts.append(data);offset+=len(data)
    (ROOT/'app-icon.ico').write_bytes(struct.pack('<HHH',0,1,len(sizes))+b''.join(entries)+b''.join(parts))
if __name__=='__main__':main()
