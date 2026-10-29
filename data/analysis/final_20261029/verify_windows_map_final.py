import os,subprocess,time,json
from pathlib import Path
out=Path('data/analysis/final_20261029')
x='/tmp/punjdrone-win-toolchain/root/usr/bin/xdotool'
env=dict(os.environ,LD_LIBRARY_PATH='/tmp/punjdrone-win-toolchain/root/usr/lib/x86_64-linux-gnu',WINEDLLPATH='/tmp/punjdrone-win-toolchain/root/usr/lib/x86_64-linux-gnu/wine',WINEPREFIX='/tmp/punjdrone-wine',WINEDEBUG='-all',DRONEID_MAP_OFFLINE='1')
def windows():
 r=subprocess.run([x,'search','--name','^DroneID Receiver$'],env=env,capture_output=True,text=True)
 return set(r.stdout.split())
before=windows();logfile=out/'windows-map-final.log'
with logfile.open('w') as log:
 app=subprocess.Popen(['/tmp/punjdrone-win-toolchain/root/usr/lib/wine/wine64','build/windows/droneid_ui.exe','--replay','data/analysis/odid_20261028/odid_ch6.iq','--odid','--secs','1','--frames','1200','--screenshot',str(out/'windows-map-final.ppm')],env=env,stdout=log,stderr=log)
 try:
  time.sleep(3);new=windows()-before;assert len(new)==1,new
  wid=new.pop();subprocess.run([x,'windowactivate','--sync',wid],env=env,check=True);time.sleep(2)
  for y in range(570,791,5):
   subprocess.run([x,'mousemove','--sync','--window',wid,'80',str(y)],env=env,check=True);time.sleep(.08)
   subprocess.run([x,'click','1'],env=env,check=True);time.sleep(.08)
   if 'ui_selection:' in logfile.read_text():break
  code=app.wait(timeout=50)
  passed=code==0 and 'drone_location=valid' in logfile.read_text()
  (out/'windows-map-final-check.json').write_text(json.dumps(dict(passed=passed,exit_code=code,tiles='offline cached',screenshot=str(out/'windows-map-final.ppm')),indent=2)+'\n')
  print('Final Windows map/selection',passed);assert passed
 finally:
  if app.poll() is None:app.terminate();app.wait()
