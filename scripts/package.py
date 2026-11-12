#!/usr/bin/env python3
"""Package only a clean, matching build; inspect final relocated runtime imports.

Default Mac output is ad-hoc signed. Release signing and notarization use
identities already configured by the publisher on the packaging machine.
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
import zipfile

from common import ROOT,TARGETS,lock,run,sha256
from write_build_info import build_info
from audit_dependencies import audit_macho,audit_pe

MODEL_LICENSE=ROOT/'licenses/PLaMo-Community-ja.txt'

def companion_files(target,model_file):
    files={MODEL_LICENSE.name:MODEL_LICENSE,'README.md':ROOT/'README.md'}
    if target.endswith('-release'):
        model=lock()['model_fixture']
        path=model_file or ROOT/model['filename']
        if not path.is_file() or sha256(path)!=model['sha256']:
            raise RuntimeError('Supply the pinned official GGUF with --model-file: '+str(path))
        files[model['filename']]=path
    return files

def verify_companions(directory,expected):
    for name,digest in expected.items():
        path=directory/name
        if not path.is_file() or sha256(path)!=digest:
            raise RuntimeError('Missing or changed package file: '+str(path))

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

def source_sbom(info, pdf):
    spec = lock()
    components = []
    for name, entry in spec['dependencies'].items():
        component = {'type': 'file' if name.startswith('Noto') else 'library', 'name': name,
                     'version': entry.get('version') or entry['revision'],
                     'externalReferences': [{'type': 'distribution', 'url': entry['url']}]}
        if 'revision' in entry:
            component['properties'] = [{'name': 'git:revision', 'value': entry['revision']}]
        if 'sha256' in entry:
            component['hashes'] = [{'alg': 'SHA-256', 'content': entry['sha256']}]
        components.append(component)
    for path, url, revision in re.findall(r'^([\w./-]+): (https://\S+)@([0-9a-f]{40})\s*$',
                                          pdf['resolved_dependencies'], re.MULTILINE):
        if path == 'pdfium':
            continue
        components.append({'type': 'library', 'name': path, 'version': revision,
                           'externalReferences': [{'type': 'vcs', 'url': url}]})
    if info['target'].endswith('-release'):
        model = spec['model_fixture']
        components.append({'type': 'machine-learning-model', 'name': model['filename'],
                           'version': model['revision'], 'hashes': [{'alg': 'SHA-256', 'content': model['sha256']}]})
    return {'bomFormat': 'CycloneDX', 'specVersion': '1.6', 'version': 1,
            'metadata': {'component': {'type': 'application', 'name': info['product'], 'version': info['version']},
                         'lifecycles': [{'phase': 'build'}]}, 'components': components}

def validate_static_windows(build,target):
    entries=json.loads((build/'compile_commands.json').read_text())
    expected='MTd' if target.endswith('-debug') else 'MT';count=0
    for entry in entries:
        if Path(entry['file']).suffix.lower() not in ('.c','.cc','.cpp','.cxx'):continue
        flags=re.findall(r'(?:^|\s)[/-](M[DT]d?)(?=\s|$)',entry['command'])
        if flags!=[expected]:raise RuntimeError('Unexpected CRT flags for '+Path(entry['file']).name+': '+str(flags))
        count+=1
    if not count:raise RuntimeError('No CRT compile evidence')
    if target.endswith('-release'):
        validate_kernel_symbols(entries)


def validate_kernel_symbols(entries):
    # COFF COMDAT selection may pick either definition of a template. The
    # portable ggml-cpu objects must not share function names with the AVX2 ones.
    groups = {True: [], False: []}
    for entry in entries:
        output = Path(entry['output'])
        if 'ggml-cpu.dir' in output.parts:
            if not output.is_absolute():
                output = Path(entry['directory']) / output
            groups['-mavx2' in entry['command']].append(output)
    if not all(groups.values()):
        raise RuntimeError('Missing AVX2 or baseline kernel objects')
    def functions(paths):
        symbols = set()
        for path in paths:
            table = subprocess.check_output(['dumpbin', '/nologo', '/symbols', path],
                                            text=True, errors='replace', env={**os.environ, 'VSLANG': '1033'})
            symbols.update(re.findall(r'\bSECT[\dA-F]+\s+notype\s+\(\)\s+External\s+\| (\S+)', table))
        if not symbols:
            raise RuntimeError('Cannot identify external COFF functions')
        return symbols
    shared = functions(groups[True]) & functions(groups[False])
    if shared:
        raise RuntimeError('ISA variants share external functions: ' + ', '.join(sorted(shared)))
def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--target',choices=TARGETS,required=True)
    parser.add_argument('--model-file',type=Path,help='Pinned official GGUF to include in a Release ZIP')
    parser.add_argument('--windows-cert',help='SHA-1 thumbprint of an Authenticode signing certificate')
    parser.add_argument('--mac-identity',help='Developer ID Application identity in the signing keychain')
    parser.add_argument('--notary-profile',help='Submit the signed Mac app using this notarytool keychain profile')
    args=parser.parse_args()
    target=args.target;mac=target.startswith('macos-');values=product();version=values['OPT_VERSION'];name=values['OPT_PRODUCT_NAME'];exe=values['OPT_EXECUTABLE_NAME']
    if mac!=(platform.system()=='Darwin'):raise SystemExit('Package on the target operating system')
    if args.windows_cert and (mac or not re.fullmatch(r'[0-9a-fA-F]{40}',args.windows_cert)):
        parser.error('--windows-cert requires a Windows target and a certificate thumbprint')
    if (args.mac_identity or args.notary_profile) and not mac:parser.error('Apple signing requires a macOS target')
    if args.notary_profile and not args.mac_identity:parser.error('--notary-profile requires --mac-identity')
    signature=('Developer ID' if args.mac_identity else 'ad-hoc') if mac else ('Authenticode' if args.windows_cert else 'unsigned')
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
    companions=companion_files(target,args.model_file)
    companion_hashes={name:sha256(path) for name,path in companions.items()}
    dist=ROOT/'dist'/target;dist.mkdir(parents=True,exist_ok=True)
    signing_variant='-notarized' if args.notary_profile else '-signed' if args.windows_cert or args.mac_identity else ''
    unique=dist/(info['identity_sha256'][:12]+signing_variant)
    if unique.exists():
        verify_companions(unique,companion_hashes)
        manifest=json.loads((unique/'package-manifest.json').read_text())
        if (manifest.get('signing_identity')!=(args.windows_cert or args.mac_identity) or
                manifest.get('signature')!=signature or manifest.get('notarized')!=bool(args.notary_profile)):
            raise SystemExit('Existing package signing identity differs from the requested identity')
        artifact=unique/manifest['artifact']
        if artifact.parent!=unique or manifest.get('identity_sha256')!=info['identity_sha256'] or sha256(artifact)!=manifest['sha256']:
            raise SystemExit('Existing package was changed; inspect it before replacing')
        if mac:
            audit_bundle(unique/(name+'.app'),'arm64' if 'arm64' in target else 'x86_64')
            run(['codesign','--verify','--deep','--strict',unique/(name+'.app')])
            if args.notary_profile:run(['xcrun','stapler','validate',unique/(name+'.app')])
        elif audit_pe(unique/(exe+'.exe'))['failures']:raise SystemExit('Existing executable dependency audit failed')
        elif args.windows_cert:run(['signtool','verify','/pa',unique/(exe+'.exe')])
        print('Verified existing package',artifact);return
    with tempfile.TemporaryDirectory(prefix='.package-',dir=dist) as temporary:
        stage=Path(temporary);payload=stage/'payload';payload.mkdir()
        for filename,path in companions.items():shutil.copyfile(path,payload/filename)
        sbom=source_sbom(info,pdf)
        write_json(payload/'SBOM.cdx.json',sbom)
        if mac:
            bundle=payload/(name+'.app');shutil.copytree(build/(name+'.app'),bundle)
            resources=bundle/'Contents/Resources'
            shutil.copyfile(ROOT/'resources/help-ja.txt',resources/'HELP-ja.txt')
            shutil.copyfile(ROOT/'THIRD_PARTY_NOTICES.txt',resources/'LICENSES.txt')
            write_json(resources/'BUILD-INFO.json',{**info,'signature':signature})
            write_json(resources/'DEPENDENCIES.json',sanitized(pdf))
            with (bundle/'Contents/Info.plist').open('rb') as source:plist=plistlib.load(source)
            if plist.get('CFBundleIdentifier')!=values['OPT_BUNDLE_ID'] or plist.get('CFBundleShortVersionString')!=version:raise RuntimeError('Bundle identity/version mismatch')
            if sha256(resources/'NotoSansCJKjp-Regular.otf')!=lock()['dependencies']['NotoSansCJKjp-Regular']['sha256']:raise RuntimeError('Bundled font mismatch')
            if not (resources/'app-icon.icns').is_file():raise RuntimeError('Product icon missing')
            arch='arm64' if 'arm64' in target else 'x86_64';reports=audit_bundle(bundle,arch)
            run(['codesign','--force','--options','runtime','--sign',args.mac_identity or '-',
                 '--timestamp' if args.mac_identity else '--timestamp=none',bundle])
            if args.notary_profile:
                submission=stage/'notary.zip'
                run(['ditto','-c','-k','--keepParent',bundle,submission])
                result=json.loads(run(['xcrun','notarytool','submit',submission,'--keychain-profile',
                                       args.notary_profile,'--wait','--output-format','json'],capture=True))
                if result.get('status')!='Accepted':raise RuntimeError('Apple notarization was not accepted')
                run(['xcrun','stapler','staple',bundle])
            run(['codesign','--verify','--deep','--strict','--verbose=2',bundle])
        else:
            validate_static_windows(build,target)
            staged_binary=payload/(exe+'.exe');shutil.copyfile(binary,staged_binary)
            if args.windows_cert:
                run(['signtool','sign','/sha1',args.windows_cert,'/fd','SHA256',staged_binary])
            reports=[audit_pe(staged_binary)]
            if reports[0]['failures']:raise RuntimeError(json.dumps(reports,indent=2))
            reports[0]['file']=staged_binary.name
            write_json(payload/'BUILD-INFO.json',info);write_json(payload/'DEPENDENCIES.json',sanitized(pdf))
        verify_companions(payload,companion_hashes)
        archive_name=f'{exe}-{version}-{target}.zip';archive=stage/archive_name
        extracted=stage/'extracted'
        if mac:
            run(['ditto','-c','-k','--sequesterRsrc',payload,archive])
            run(['ditto','-x','-k',archive,extracted])
            restored=extracted/bundle.name;run(['codesign','--verify','--deep','--strict','--verbose=2',restored]);audit_bundle(restored,arch)
            if args.notary_profile:run(['xcrun','stapler','validate',restored])
            restored_binary=restored/'Contents/MacOS'/name
        else:
            with zipfile.ZipFile(archive,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as output:
                for path in sorted(payload.iterdir()):output.write(path,path.name)
            with zipfile.ZipFile(archive) as output:output.extractall(extracted)
            restored_binary=extracted/binary.name
            if audit_pe(restored_binary)['failures']:raise RuntimeError('Extracted executable dependency audit failed')
            if args.windows_cert:run(['signtool','verify','/pa',restored_binary])
        verify_companions(extracted,companion_hashes)
        if json.loads((extracted/'SBOM.cdx.json').read_text(encoding='utf-8'))!=sbom:
            raise RuntimeError('ZIP round-trip changed dependency inventory')
        packaged_binary=bundle/'Contents/MacOS'/name if mac else staged_binary
        if sha256(restored_binary)!=sha256(packaged_binary):raise RuntimeError('ZIP round-trip changed executable')
        shutil.move(str(archive),str(payload/archive_name));artifact=payload/archive_name
        manifest={**info,'artifact':artifact.name,'size':artifact.stat().st_size,'sha256':sha256(artifact),
          'files':{name:{'sha256':companion_hashes[name],'size':path.stat().st_size} for name,path in companions.items()},
          'runtime_audit':reports,'model_included':target.endswith('-release'),'clean_machine_execution':'not implied by packaging',
          'signature':signature,'signing_identity':args.windows_cert or args.mac_identity,'notarized':bool(args.notary_profile)}
        write_json(payload/'package-manifest.json',manifest)
        (payload/(artifact.name+'.sha256')).write_text(manifest['sha256']+'  '+artifact.name+'\n')
        shutil.move(str(payload),str(unique))
    print('Created',unique/manifest['artifact']);print('SHA-256',manifest['sha256'])
if __name__=='__main__':main()
