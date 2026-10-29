"""Visible X11 GUI click checks at the application's default 1280x800 size."""
import os,subprocess,time,json
from x11_click import click
from pathlib import Path
out=Path('data/analysis/final_20261029')
xdotool='/tmp/punjdrone-win-toolchain/root/usr/bin/xdotool'
xenv=dict(os.environ,LD_LIBRARY_PATH='/tmp/punjdrone-win-toolchain/root/usr/lib/x86_64-linux-gnu')
wine='/tmp/punjdrone-win-toolchain/root/usr/lib/wine/wine64'
wenv=dict(xenv,WINEDLLPATH='/tmp/punjdrone-win-toolchain/root/usr/lib/x86_64-linux-gnu/wine',WINEPREFIX='/tmp/punjdrone-wine',WINEDEBUG='-all')
cases=[
 ('native-pilot-only',[],['--replay','tests/data/mini2_wideband.cf32','--format','cf32','--rate','50000000','--auto-center'],602,'drone_location=unavailable center=31.1471000,75.3412000 zoom=7'),
 ('native-unsupported',[],['--replay','data/analysis/corpus_deep_20261026/002-995552cb67.decoded.cf32','--freq-offset','-11500000'],602,'drone_location=unavailable center=31.1471000,75.3412000 zoom=7'),
 ('native-valid-location',[],['--replay','data/analysis/odid_20261028/odid_ch6.iq','--odid','--secs','1'],620,'drone_location=valid center=51.4775410,-0.0015244 zoom=14'),
 ('windows-valid-location',[wine],['--replay','data/analysis/odid_20261028/odid_ch6.iq','--odid','--secs','1'],620,'drone_location=valid center=51.4775410,-0.0015244 zoom=14')]
results=[]
for name,runner,args,y,expected in cases[:2]:
 env=wenv if runner else os.environ.copy()
 binary='build/windows/droneid_ui.exe' if runner else 'build/droneid_ui'
 logfile=out/(name+'.log'); shot=out/(name+'.ppm')
 with logfile.open('w') as log:
  app=subprocess.Popen(runner+[binary]+args+['--frames','600','--screenshot',str(shot)],env=env,stdout=log,stderr=log)
  try:
   time.sleep(3)
   ids=subprocess.check_output([xdotool,'search','--all','--pid',str(app.pid),'--name','^DroneID Receiver$'],env=xenv,text=True).split()
   assert len(ids)==1,ids
   wid=ids[0]
   subprocess.run([xdotool,'windowactivate','--sync',wid],env=xenv,check=True)
   time.sleep(2) # compositor positioning must finish before relative pointer coordinates
   click(int(wid),50,y)
   code=app.wait(timeout=55)
   passed=code==0 and expected in logfile.read_text() and shot.is_file()
   results.append(dict(name=name,passed=passed,exit_code=code,expected_selection=expected,screenshot=str(shot)))
   print(name,'PASS' if passed else 'FAIL',flush=True)
  finally:
   if app.poll() is None: app.terminate();app.wait()
 (out/'map-native-retry-checks.json').write_text(json.dumps(results,indent=2)+'\n')
assert all(r['passed'] for r in results)
