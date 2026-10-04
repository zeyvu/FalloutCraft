#!/usr/bin/env python3
"""Checks that edits to fabric/src only ADDED preprocessor lines (//#if, //#elif, //#else, //#endif,
//$$ ...) and left every other line exactly as in git HEAD (so the 26.x builds are unchanged).
Usage: python3 primary_unchanged.py [files...]   (default: all changed .java under fabric/src)"""
import re, subprocess, sys
ROOT='/home/claude/fc'
def strip(text):
    return [l for l in text.split('\n') if not re.match(r'^\s*//(#(if|elif|else|endif)\b|\$\$)', l)]
files=sys.argv[1:] or subprocess.run(['git','-C',ROOT,'diff','--name-only','HEAD','--','fabric/src'],capture_output=True,text=True).stdout.split()
bad=0
for f in files:
    if not f.endswith('.java'): continue
    try: old=subprocess.run(['git','-C',ROOT,'show','HEAD:'+f],capture_output=True,text=True,check=True).stdout
    except subprocess.CalledProcessError: print('new file (fine):',f); continue
    new=open(ROOT+'/'+f).read()
    if strip(new)!=strip(old):
        bad+=1; print('CHANGED LIVE CODE:',f)
        import difflib
        for d in list(difflib.unified_diff(strip(old),strip(new),lineterm='',n=0))[:20]: print('   ',d)
print('ok' if not bad else f'{bad} file(s) changed live 26.x code')
