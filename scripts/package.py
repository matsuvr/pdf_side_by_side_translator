#!/usr/bin/env python3
"""Package only a clean, matching build; inspect final relocated runtime imports.

Mac output is an ad-hoc signed development app/ZIP, NOT notarized. This script
does not use signing credentials, submit to Apple or download a model.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import plistlib
import re
import shutil
import subprocess
import tempfile

from common import ROOT,TARGETS,lock,run,sha256
from write_build_info import build_info
from audit_dependencies import audit_macho,audit_pe

def product():
    values={}
    for key,value in re.findall(r'set\((OPT_[A-Z_]+) "([^"]+)"\)',(ROOT/'cmake/Product.cmake').read_text()):values[key]=value
    return values
def sanitized(value):
    if isinstance(value,str):return value.replace(str(ROOT),'${SOURCE_ROOT}')
    if isinstance(value,list):return [sanitized(x) for x in value]
    if isinstance(value,dict):return {k:sanitized(v) for k,v in value.items()}
    return value
def audit_bundle(bundle,arch):
    reports=[]
    for path in sorted(bundle.rglob('*')):
        if path.is_symlink():raise RuntimeError('Unexpected bundle symlink: '+str(path.relative_to(bundle)))
        if not path.is_file():continue
        if path.suffix.lower() in {'.gguf','.py','.dll','.dylib','.metallib'}:raise RuntimeError('Unexpected runtime sidecar: '+path.name)
        magic=path.read_bytes()[:4]
        if magic in (b'\xcf\xfa\xed\xfe',b'\xfe\xed\xfa\xcf',b'\xca\xfe\xba\xbe',b'\xbe\xba\xfe\xca'):
            report=audit_macho(path,arch,'14.0');report['file']=str(path.relative_to(bundle));reports.append(report)
    if len(reports)!=1:raise RuntimeError('Expected exactly one product Mach-O')
    if any(r['failures'] for r in reports):raise RuntimeError(json.dumps(reports,indent=2))
    return reports
def write_json(path,value):path.write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
def validate_static_windows(build,target):
    entries=json.loads((build/'compile_commands.json').read_text())
    expected='MTd' if target.endswith('-debug') else 'MT';count=0
    for entry in entries:
        if Path(entry['file']).suffix.lower() not in ('.c','.cc','.cpp','.cxx'):continue
        flags=re.findall(r'(?:^|\s)[/-](M[DT]d?)(?=\s|$)',entry['command'])
        if flags!=[expected]:raise RuntimeError('Unexpected CRT flags for '+Path(entry['file']).name+': '+str(flags))
        count+=1
    if not count:raise RuntimeError('No CRT compile evidence')
def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--target',choices=TARGETS,required=True);args=parser.parse_args()
    target=args.target;mac=target.startswith('macos-');values=product();version=values['OPT_VERSION'];name=values['OPT_PRODUCT_NAME'];exe=values['OPT_EXECUTABLE_NAME']
    if mac!=(platform.system()=='Darwin'):raise SystemExit('Package on the target operating system')
    info=build_info(target,version)
    if info['dirty']:raise SystemExit('Commit reviewed source before packaging; working tree is modified')
    build=ROOT/'build'/target
    if not (build/'BUILD-INFO.json').exists() or json.loads((build/'BUILD-INFO.json').read_text())!=info:
        raise SystemExit('Build identity is stale. Run cmake --build --preset '+target)
    pdf=json.loads((ROOT/'.work/pdfium'/target/'pdfium-build-manifest.json').read_text())
    if pdf['revision']!=lock()['dependencies']['PDFium']['revision'] or pdf['target']!=target:raise SystemExit('PDFium manifest differs from lock/target')
    run([os.sys.executable,ROOT/'scripts/collect_notices.py','--check'])
    binary=build/(name+'.app')/'Contents/MacOS'/name if mac else build/(exe+'.exe')
    if ('OPT_BUILD_INFO:'+info['identity_sha256']).encode() not in binary.read_bytes():raise SystemExit('Executable does not contain the matching build identity')
    dist=ROOT/'dist'/target;dist.mkdir(parents=True,exist_ok=True)
    unique=dist/info['identity_sha256'][:12]
    if unique.exists():
        manifest=json.loads((unique/'package-manifest.json').read_text())
        artifact=unique/manifest['artifact']
        if artifact.parent!=unique or manifest.get('identity_sha256')!=info['identity_sha256'] or sha256(artifact)!=manifest['sha256']:
            raise SystemExit('Existing package was changed; inspect it before replacing')
        if mac:
            audit_bundle(unique/(name+'.app'),'arm64' if 'arm64' in target else 'x86_64')
            run(['codesign','--verify','--deep','--strict',unique/(name+'.app')])
        elif audit_pe(artifact)['failures']:raise SystemExit('Existing executable dependency audit failed')
        print('Verified existing package',artifact);return
    with tempfile.TemporaryDirectory(prefix='.package-',dir=dist) as temporary:
        stage=Path(temporary);payload=stage/'payload';payload.mkdir()
        if mac:
            bundle=payload/(name+'.app');shutil.copytree(build/(name+'.app'),bundle)
            resources=bundle/'Contents/Resources'
            shutil.copyfile(ROOT/'resources/help-ja.txt',resources/'HELP-ja.txt')
            shutil.copyfile(ROOT/'THIRD_PARTY_NOTICES.txt',resources/'LICENSES.txt')
            write_json(resources/'BUILD-INFO.json',{**info,'signature':'ad-hoc development signature','notarized':False})
            write_json(resources/'DEPENDENCIES.json',sanitized(pdf))
            with (bundle/'Contents/Info.plist').open('rb') as source:plist=plistlib.load(source)
            if plist.get('CFBundleIdentifier')!=values['OPT_BUNDLE_ID'] or plist.get('CFBundleShortVersionString')!=version:raise RuntimeError('Bundle identity/version mismatch')
            if sha256(resources/'NotoSansCJKjp-Regular.otf')!=lock()['dependencies']['NotoSansCJKjp-Regular']['sha256']:raise RuntimeError('Bundled font mismatch')
            if not (resources/'app-icon.icns').is_file():raise RuntimeError('Product icon missing')
            arch='arm64' if 'arm64' in target else 'x86_64';reports=audit_bundle(bundle,arch)
            run(['codesign','--force','--sign','-','--timestamp=none',bundle])
            run(['codesign','--verify','--deep','--strict','--verbose=2',bundle])
            archive_name=f'{exe}-{version}-{target}.zip';archive=stage/archive_name
            run(['ditto','-c','-k','--sequesterRsrc','--keepParent',bundle,archive])
            extracted=stage/'extracted';run(['ditto','-x','-k',archive,extracted])
            restored=extracted/bundle.name;run(['codesign','--verify','--deep','--strict','--verbose=2',restored]);audit_bundle(restored,arch)
            if sha256(restored/'Contents/MacOS'/name)!=sha256(bundle/'Contents/MacOS'/name):raise RuntimeError('ZIP round-trip changed executable')
            shutil.move(str(archive),str(payload/archive_name));artifact=payload/archive_name
        else:
            validate_static_windows(build,target)
            artifact=payload/(exe+'.exe');shutil.copyfile(binary,artifact);reports=[audit_pe(artifact)]
            if reports[0]['failures']:raise RuntimeError(json.dumps(reports,indent=2))
            reports[0]['file']=artifact.name
            write_json(payload/'BUILD-INFO.json',info);write_json(payload/'DEPENDENCIES.json',sanitized(pdf))
        manifest={**info,'artifact':artifact.name,'size':artifact.stat().st_size,'sha256':sha256(artifact),
          'runtime_audit':reports,'model_included':False,'clean_machine_execution':'not implied by packaging',
          'signature':'ad-hoc; not notarized' if mac else 'unsigned'}
        write_json(payload/'package-manifest.json',manifest)
        (payload/(artifact.name+'.sha256')).write_text(manifest['sha256']+'  '+artifact.name+'\n')
        shutil.move(str(payload),str(unique))
    print('Created',unique/manifest['artifact']);print('SHA-256',manifest['sha256'])
if __name__=='__main__':main()
