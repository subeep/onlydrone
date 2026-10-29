import os,sys,subprocess,time,json
from pathlib import Path
from x11_click import click
out=Path('data/analysis/final_20261029')
x='/tmp/punjdrone-win-toolchain/root/usr/bin/xdotool'
xenv=dict(os.environ,LD_LIBRARY_PATH='/tmp/punjdrone-win-toolchain/root/usr/lib/x86_64-linux-gnu')
logfile=out/'live-map.log'
with logfile.open('w') as log:
 app=subprocess.Popen(['build/droneid_ui','--device','addr=192.168.10.2','--odid','--freq','2437000000','--gain','20','--frames','1800','--screenshot',str(out/'live-map.ppm')],stdout=log,stderr=log)
 try:
  time.sleep(8)
  ids=subprocess.check_output([x,'search','--all','--pid',str(app.pid),'--name','^DroneID Receiver$'],env=xenv,text=True).split()
  assert len(ids)==1,ids
  wid=ids[0]
  subprocess.run([x,'windowactivate','--sync',wid],env=xenv,check=True)
  time.sleep(1)
  for y in range(550,701,4):
   click(int(wid),50,y)
   if 'ui_selection:' in logfile.read_text(): break
  code=app.wait(timeout=70)
  passed=code==0 and 'drone_location=valid' in logfile.read_text()
  (out/'live-map-check.json').write_text(json.dumps({'passed':passed,'exit_code':code,'input':'Live X310 20 MSPS / 2437 MHz / RX2 / gain 20','selection_log':str(logfile),'screenshot':str(out/'live-map.ppm'),'interaction':'X11 client-coordinate mouse events into actual GLFW packet table'},indent=2)+'\n')
  print('Live map selection', 'PASS' if passed else 'FAIL')
  assert passed
 finally:
  if app.poll() is None:app.terminate();app.wait()
