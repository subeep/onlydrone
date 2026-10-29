#!/usr/bin/env python3
"""Package built x64 binaries and a matching source snapshot without IQ downloads."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build',type=Path,default=Path('build/windows'))
    parser.add_argument('--deps',type=Path,required=True,help='Directory containing pinned GLFW/FFTW archives and extracted sources')
    parser.add_argument('--toolchain',type=Path,help='Optional extracted Debian MinGW root for runtime copyright notices')
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[1]
    dist=root/'dist'; dist.mkdir(exist_ok=True)
    package=dist/'punjdrone-windows-x64'; package.mkdir(exist_ok=True)
    tools=package/'tools'; tools.mkdir(exist_ok=True)
    for name in ['droneid_ui','droneid_sweep','droneid_hop','droneid_decode','odid_decode',
                 'o4_detect','rf_analyze','iq_record','iq_record_pair','df_bearing',
                 'droneid','iq_replay','iq_gen','frame_gen','odid_gen','burst_scan','channelize']:
        shutil.copy2(args.build/(name+'.exe'),(package if name=='droneid_ui' else tools)/(name+'.exe'))
    # An existing distribution must not silently retain executables from an older build.
    for binary in package.rglob('*.exe'):
        current = args.build/binary.name
        if not current.is_file() or binary.read_bytes() != current.read_bytes():
            raise RuntimeError(f'packaged executable does not match current build: {binary}')
    for name in ['COPYING','THIRD_PARTY.md','progress.md']:
        shutil.copy2(root/name,package/name)
    for name in ['WINDOWS.md','WINDOWS_RUN_GUIDE.md','AUTOMATIC_SWEEP.md','O4_DETECTOR.md','PACKET_MAP.md','FINAL_VERIFICATION.md']:
        shutil.copy2(root/'docs'/name,package/name)
    licenses=package/'licenses'; licenses.mkdir(exist_ok=True)
    notices={'imgui.txt':root/'third_party/imgui/LICENSE.txt',
             'turbofec.txt':root/'third_party/turbofec/LICENSE',
             'CRCpp.txt':root/'third_party/crcpp/LICENSE',
             'GLFW.txt':args.deps/'glfw-3.4/LICENSE.md',
             'FFTW.txt':args.deps/'fftw-3.3.10/COPYING',
             'UHD-headers.txt':root/'third_party/uhd_c/README.md',
             'stb_image.txt':root/'third_party/stb/LICENSE'}
    if args.toolchain:
        for component in ['gcc-mingw-w64-base','mingw-w64-common']:
            notices[component+'.txt']=args.toolchain/'usr/share/doc'/component/'copyright'
    for name,path in notices.items(): shutil.copy2(path,licenses/name)
    example=package/'example'; example.mkdir(exist_ok=True)
    for name in ['mini2_wideband.cf32','README.md','LICENSE.DroneSecurity']:
        shutil.copy2(root/'tests/data'/name,example/name)
    (package/'Replay-example.cmd').write_text('@echo off\ncd /d "%~dp0"\ndroneid_ui.exe --replay "example\\mini2_wideband.cf32" --format cf32 --rate 50000000 --auto-center\n')
    (package/'Sweep-plan.cmd').write_text('@echo off\ncd /d "%~dp0"\ntools\\droneid_sweep.exe --profile balanced\npause\n')
    (package/'README.txt').write_text('Extract this entire folder. Double-click droneid_ui.exe.\n'
        'Use IQ replay, Live X310, or Automatic sweep in Source settings.\n'
        'Click a decoded packet for its details and map location. Map defaults to Punjab; uncached tiles need internet. Read PACKET_MAP.md.\n'
        'Live modes require a compatible 64-bit UHD runtime. No UHD DLL is bundled.\n'
        'Read WINDOWS.md, AUTOMATIC_SWEEP.md and FINAL_VERIFICATION.md. Full model/channel coverage acceptance is pending.\n'
        'O4 results are candidates, not confirmed identities. O4+ and physical DF are not complete.\n'
        'Replay-example.cmd uses a small licensed reference excerpt. Sweep-plan.cmd does not access the radio.\n'
        'Keep the matching punjdrone-windows-source.zip and licenses if redistributing.\n')
    (package/'SOURCE.txt').write_text('Matching source: ../punjdrone-windows-source.zip\n'
        'Includes the working source snapshot, build scripts, tests, licenses and pinned GLFW/FFTW source archives.\n'
        'This binary includes local changes beyond the base Git commit; use the snapshot and manifest.\n')
    source=dist/'punjdrone-windows-source.zip'
    source_hashes={}
    source_files=list(root.glob('*.md'))+[root/'CMakeLists.txt',root/'COPYING']
    for folder in ['src','include','tools','tests','cmake','config','scripts','third_party']:
        source_files += [p for p in (root/folder).rglob('*') if p.is_file() and not any(
            x in p.parts for x in ['.git','__pycache__']) and p.suffix not in ['.pyc','.o','.a','.exe','.so']]
    source_files += list((root/'docs').rglob('*.md'))
    with zipfile.ZipFile(source,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as archive:
        for path in sorted(set(source_files)):
            relative=str(path.relative_to(root)); archive.write(path,'punjdrone-source/'+relative)
            source_hashes[relative]=hashlib.sha256(path.read_bytes()).hexdigest()
        for name in ['glfw-3.4.tar.gz','fftw-3.3.10.tar.gz']:
            path=args.deps/name
            archive.write(path,'punjdrone-source/dependencies/'+name)
            source_hashes['dependencies/'+name]=hashlib.sha256(path.read_bytes()).hexdigest()
        archive.writestr('punjdrone-source/SOURCE_SHA256.json',json.dumps(source_hashes,indent=2)+'\n')
    manifest=dict(base_commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),
                  snapshot_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                  binaries={str(p.relative_to(package)):hashlib.sha256(p.read_bytes()).hexdigest() for p in package.rglob('*.exe')})
    (package/'BUILD_MANIFEST.json').write_text(json.dumps(manifest,indent=2)+'\n')
    binary=dist/'punjdrone-windows-x64.zip'
    with zipfile.ZipFile(binary,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as archive:
        for path in sorted(package.rglob('*')):
            if path.is_file(): archive.write(path,str(path.relative_to(dist)))
    (dist/'SHA256SUMS.txt').write_text(''.join(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n' for p in [binary,source]))
    for path in [binary,source]:
        with zipfile.ZipFile(path) as archive:
            if archive.testzip() is not None: raise RuntimeError('corrupt archive')
        print(path, path.stat().st_size)


if __name__=='__main__': main()
