#!/usr/bin/env python3
"""Collect exact pinned dependency notices. --check detects source/notice drift.

The set follows the PDFium GN closure and the wx core/base + llama static link
closure, rather than every downloaded build tool. Some platform-conditional
components are included for both distribution targets. Model terms are included
for reference and output use; the model itself is never packaged.
"""
import argparse
import hashlib
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
ROOTS={'project':ROOT,'pdfium':ROOT/'.work/pdfium-workspace/pdfium','wx':ROOT/'.work/src/wxWidgets','llama':ROOT/'.work/src/llama.cpp','resources':ROOT/'.work/resources'}
# Full upstream documents, without removing disclaimers or exception clauses.
PLAN=[
 ('Application original source/resources','project',['LICENSE']),
 ('PDFium, including embedded Foxit font/CMap data','pdfium',['LICENSE']),
 ('Chromium support and PartitionAlloc','project',['licenses/Chromium-BSD.txt']),
 ('PDFium / Anti-Grain Geometry 2.3','pdfium',['third_party/agg23/copying']),
 ('PDFium / Abseil','pdfium',['third_party/abseil-cpp/LICENSE']),
 ('PDFium / Brotli decoder','pdfium',['third_party/brotli/LICENSE']),
 ('PDFium / Dragonbox (Boost alternative selected)','pdfium',['third_party/dragonbox/src/LICENSE-Boost']),
 ('PDFium / fast_float (MIT alternative selected)','pdfium',['third_party/fast_float/src/LICENSE-MIT']),
 ('PDFium / FreeType (FTL alternative selected)','pdfium',['third_party/freetype/FTL.TXT']),
 ('PDFium / Little CMS','pdfium',['third_party/lcms/src/LICENSE']),
 ('PDFium / OpenJPEG','pdfium',['third_party/libopenjpeg/src/LICENSE']),
 ('PDFium / HarfBuzz','pdfium',['third_party/harfbuzz/src/COPYING','third_party/harfbuzz/src/src/ms-use/COPYING']),
 ('PDFium / Highway','pdfium',['third_party/highway/src/LICENSE']),
 ('PDFium / ICU and Unicode data','pdfium',['third_party/icu/LICENSE']),
 ('PDFium / libjpeg-turbo','pdfium',['third_party/libjpeg_turbo/LICENSE.md','third_party/libjpeg_turbo/README.ijg']),
 ('PDFium / libyuv','pdfium',['third_party/libyuv/LICENSE']),
 ('PDFium / zlib','pdfium',['third_party/zlib/LICENSE']),
 ('PDFium / LLVM libc++','pdfium',['third_party/libc++/src/LICENSE.TXT']),
 ('PDFium / LLVM libc++abi','pdfium',['third_party/libc++abi/src/LICENSE.TXT']),
 ('PDFium / LLVM libc subset','pdfium',['third_party/llvm-libc/src/LICENSE.TXT']),
 ('LLVM compiler runtime','project',['licenses/LLVM-compiler-rt.txt']),
 ('wxWidgets core/base (wxWindows exception applies)','wx',['docs/licence.txt','docs/lgpl.txt']),
 ('wxWidgets / PCRE2','wx',['3rdparty/pcre/LICENCE.md','3rdparty/pcre/deps/sljit/LICENSE']),
 ('wxWidgets / libpng','wx',['src/png/LICENSE']),
 ('wxWidgets / libtiff','wx',['src/tiff/LICENSE.md']),
 ('wxWidgets / Independent JPEG Group','wx',['src/jpeg/README']),
 ('wxWidgets / zlib','wx',['src/zlib/LICENSE']),
 ('wxWidgets / libwebp and SharpYUV','wx',['3rdparty/libwebp/COPYING']),
 ('llama.cpp and ggml','llama',['LICENSE']),
 ('llama.cpp / xxHash','llama',['vendor/hash/xxhash/LICENSE']),
 ('llama.cpp / SHA-256','llama',['vendor/hash/sha256/LICENSE']),
 ('llama.cpp / rotate-bits','llama',['vendor/hash/rotate-bits/LICENSE.md']),
 ('SQLite public-domain declaration','project',['licenses/SQLite-public-domain.txt']),
 ('Noto Sans CJK JP Regular font','resources',['NotoSansCJK-LICENSE.txt']),
 ('PLaMo Community License: reference terms for the separately supplied model','project',['licenses/PLaMo-Community-ja.txt','licenses/PLaMo-Community-en.txt']),
]

def outputs():
    lock=json.loads((ROOT/'deps/third_party.lock.json').read_text())
    heading='''Offline PDF Translator — Licenses and third-party notices

Original application source, fixtures and icon: MIT License (see below).
Third-party components retain their own licenses. Model weights are NOT
distributed and are NOT covered by the application's MIT License.

Built with PLaMo
Supported separate model: PLaMo 3 Translate 0.8B (pfnet).
Model use and publication/provision of model outputs are subject to the PLaMo
Community License reproduced at the end for offline reference. In particular,
models trained using outputs are subject to that agreement. Consult the full
terms and the official model distribution before use.

This software is based in part on the work of the Independent JPEG Group.
Portions of this software are copyright © The FreeType Project
(www.freetype.org). All rights reserved.
PDFium's embedded font and CMap source includes original code copyright
2014 Foxit Software Inc., under PDFium's stated license.

The following documents are reproduced verbatim from the recorded inputs.
Optional alternatives are explicitly identified in the section names.
Pinned source revisions are recorded in deps/third_party.lock.json; PDFium's
transitive commits follow that revision's DEPS. Complete input hashes and
source paths are recorded in deps/licenses.lock.json. No upstream source
patches are applied by this application.
'''
    records=[];text=[heading]
    for component,root,files in PLAN:
        text.extend(['\n'+'='*76+'\n',component+'\n','='*76+'\n'])
        for name in files:
            data=(ROOTS[root]/name).read_bytes()
            encoding='iso-8859-1' if (root,name)==('pdfium','third_party/freetype/FTL.TXT') else 'utf-8-sig'
            body=data.decode(encoding)
            records.append({'component':component,'root':root,'path':name,'encoding':encoding,'sha256':hashlib.sha256(data).hexdigest()})
            text.extend(['\n--- '+name+' ---\n\n',body,'\n'])
    notice=''.join(text).encode('utf-8')
    manifest={'schema':1,'dependency_lock_sha256':hashlib.sha256((ROOT/'deps/third_party.lock.json').read_bytes()).hexdigest(),
      'notices_sha256':hashlib.sha256(notice).hexdigest(),'inputs':records,
      'origins':{'pdfium':lock['dependencies']['PDFium'],'wx':lock['dependencies']['wxWidgets'],'llama':lock['dependencies']['llama.cpp'],
       'LLVM-compiler-rt':'https://raw.githubusercontent.com/llvm/llvm-project/e063be79/compiler-rt/LICENSE.TXT',
       'Chromium-BSD':'https://chromium.googlesource.com/chromium/src/+/refs/heads/main/LICENSE (text SHA-256 fixed in inputs)',
       'model_license':{'repository':lock['model_fixture']['repository'],'revision':lock['model_fixture']['revision'],'paths':['LICENSE/ja','LICENSE/en']}}}
    return notice,(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n').encode('utf-8')

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--check',action='store_true');args=parser.parse_args()
    notice,manifest=outputs()
    for path,data in [(ROOT/'THIRD_PARTY_NOTICES.txt',notice),(ROOT/'deps/licenses.lock.json',manifest)]:
        if args.check:
            if not path.exists() or path.read_bytes()!=data:raise SystemExit(f'Notice/input drift: {path.name}; inspect licenses before regenerating')
        else:path.write_bytes(data)
    print(f'Verified {sum(len(files) for _,_,files in PLAN)} license inputs; notices {len(notice)} bytes')
if __name__=='__main__':main()
