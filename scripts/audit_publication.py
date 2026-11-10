#!/usr/bin/env python3
"""Read-only review of the branch to publish, including removed historical blobs.

Reports locations and pattern names, never the matched credential bytes.
This supplements manual provenance/license review; it is not a security proof.
Ignored local build/model/state data are not read or published by this tool.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess

PATTERNS={
 'private-key':rb'-----BEGIN (?:[A-Z]+ )?PRIVATE KEY-----',
 'github-token':rb'\b(?:gh[pousr]_[A-Za-z0-9_]{30,}|github_pat_[A-Za-z0-9_]{30,})\b',
 'aws-access-key':rb'\b(?:AKIA|ASIA)[A-Z0-9]{16}\b',
 'openai-key':rb'\bsk-(?:proj-|svcacct-)?[A-Za-z0-9_-]{32,}\b',
 'slack-token':rb'\bxox[baprs]-[A-Za-z0-9-]{20,}\b',
 'google-api-key':rb'\bAIza[A-Za-z0-9_-]{35}\b',
 'credential-url':rb'https?://[^/\s:@]+:[^@\s/]+@',
}
def forbidden_path(name):
    p=Path(name);parts={x.lower() for x in p.parts};suffix=p.suffix.lower()
    if parts&{'.work','.tools','build','dist','.aws','.ssh','__pycache__'}:return True
    if p.name.lower().startswith('.env') and p.name!='.env.example':return True
    if suffix in {'.gguf','.sqlite3','.pem','.p12','.pfx','.key','.log','.pyc'} or '.sqlite3-' in p.name.lower():return True
    return suffix=='.pdf' and not name.startswith('tests/fixtures/')
def content_findings(data):
    result=[]
    for label,expression in PATTERNS.items():
        for match in re.finditer(expression,data):result.append({'pattern':label,'line':data[:match.start()].count(b'\n')+1})
    return result
def git(root,*args):return subprocess.check_output(['git',*args],cwd=root)
def audit_history(root,ref='main',worktree=True):
    commits=git(root,'rev-list',ref).splitlines();blobs={};findings=[]
    # rev-list --objects gives only a representative name for reused blobs.
    # Inspect every tree so a forbidden historical alias is not overlooked.
    for commit in commits:
        for record in git(root,'ls-tree','-r','-z',commit.decode()).split(b'\0'):
            if not record:continue
            metadata,name=record.split(b'\t',1);mode,kind,sha=metadata.split()
            name=name.decode('utf-8');sha=sha.decode()
            if kind!=b'blob':continue
            if mode==b'120000':findings.append({'path':name,'pattern':'source symlink requires manual review'})
            blobs.setdefault(sha,set()).add(name)
    for sha,names in blobs.items():
        names=sorted(names)
        for name in names:
            if forbidden_path(name):findings.append({'object':sha,'path':name,'pattern':'runtime/private artifact path'})
        size=int(git(root,'cat-file','-s',sha))
        if size>10*1024*1024:findings.append({'object':sha,'path':names[0],'pattern':'unexpected source blob over 10 MiB'});continue
        for finding in content_findings(git(root,'cat-file','blob',sha)):
            findings.append({'object':sha,'path':names[0],**finding})
    live=0
    if worktree:
        names=set(git(root,'ls-files','-z').split(b'\0')+git(root,'ls-files','--others','--exclude-standard','-z').split(b'\0'))
        for raw in sorted(n for n in names if n):
            name=raw.decode('utf-8');path=root/name
            if forbidden_path(name):findings.append({'path':name,'pattern':'runtime/private artifact path'})
            if not path.is_file():continue
            live+=1
            if path.stat().st_size>10*1024*1024:findings.append({'path':name,'pattern':'unexpected source file over 10 MiB'});continue
            findings.extend({'path':name,**f} for f in content_findings(path.read_bytes()))
    emails=git(root,'log',ref,'--format=%ae%x00%ce').decode().replace('\n','\0').split('\0')
    return {'ref':ref,'commits':len(commits),'historical_blobs':len(blobs),'worktree_files':live,
        'all_authors_github_noreply':all(e.endswith('@users.noreply.github.com') for e in emails if e),'findings':findings}
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--ref',default='main');parser.add_argument('--require-noreply',action='store_true');parser.add_argument('--output',type=Path);args=parser.parse_args()
    root=Path(__file__).resolve().parents[1];report=audit_history(root,args.ref)
    data=json.dumps(report,ensure_ascii=False,indent=2)+'\n'
    if args.output:args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(data,encoding='utf-8')
    print(data,end='')
    if report['findings'] or (args.require_noreply and not report['all_authors_github_noreply']):raise SystemExit(1)
if __name__=='__main__':main()
