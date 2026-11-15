import re,subprocess,sys,os,shlex
os.makedirs('pre',exist_ok=True)
targets=subprocess.check_output(['ninja','-C','build','-t','targets','all'],text=True).splitlines()
wanted=['src/config.cpp.obj','src/nvhttp.cpp.obj','src/logging.cpp.obj','src/rtsp.cpp.obj','src/stream.cpp.obj','src/video.cpp.obj','src/platform/windows/display_vram.cpp.obj','src/confighttp.cpp.obj']
for w in wanted:
    t=[l.split(':')[0] for l in targets if l.split(':')[0].endswith(w)]
    if not t:
        print('no target for',w); continue
    t=t[0]
    out=subprocess.check_output(['ninja','-C','build','-t','commands',t],text=True).strip().splitlines()[-1]
    # drop dependency-file options and the object output, preprocess to a file instead
    out=re.sub(r' -MD -MT \S+ -MF \S+','',out)
    name=w.replace('/','_')+'.i'
    out=re.sub(r' -o \S+',' -E -o ../pre/'+name,out,count=1)
    out=re.sub(r' -c ',' ',out,count=1)
    # without the ccache launcher: the raw compiler
    out=re.sub(r'^\S*ccache(\.exe)? ','',out)
    open('pre/'+w.replace('/','_')+'.cmd','w').write(out)
    r=subprocess.run(['bash','-c',out],cwd='build',capture_output=True,text=True); print(out[:200], '...', out[-200:], r.stdout[:200])
    print(w,'rc',r.returncode,os.path.getsize('pre/'+name) if os.path.exists('pre/'+name) else 'missing',r.stderr[:200])
