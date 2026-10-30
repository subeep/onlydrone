import os,subprocess,time,json
from pathlib import Path

out=Path('data/analysis/final_20261029');logfile=out/'live-reconnect.log'
x='/tmp/punjdrone-win-toolchain/root/usr/bin/xdotool'
env=dict(os.environ,LD_LIBRARY_PATH='/tmp/punjdrone-win-toolchain/root/usr/lib/x86_64-linux-gnu')
def click(wid,xpos,ypos):
 subprocess.run([x,'mousemove','--sync','--window',str(wid),str(xpos),str(ypos)],env=env,check=True)
 time.sleep(.4)
 subprocess.run([x,'click','1'],env=env,check=True)
 time.sleep(.4)
with logfile.open('w') as log:
 app=subprocess.Popen(['build/droneid_ui','--device','addr=192.168.10.2','--odid','--freq','2437000000','--gain','20','--frames','2400','--screenshot',str(out/'live-reconnect.ppm')],stdout=log,stderr=log)
 try:
  time.sleep(7)
  ids=subprocess.check_output([x,'search','--all','--pid',str(app.pid),'--name','^DroneID Receiver$'],env=env,text=True).split();assert len(ids)==1,ids
  wid=int(ids[0]);subprocess.run([x,'windowactivate','--sync',str(wid)],env=env,check=True);time.sleep(1)
  click(wid,113,135);time.sleep(2) # Disconnect
  click(wid,40,135);time.sleep(8) # Connect
  click(wid,188,135)             # Reconnect
  code=app.wait(timeout=65)
  starts=logfile.read_text().count('UsrpSource: opening')
  passed=code==0 and starts==3
  (out/'live-reconnect-check.json').write_text(json.dumps(dict(passed=passed,exit_code=code,radio_open_count=starts,actions=['automatic start','Disconnect','Connect','Reconnect','window close'],screenshot=str(out/'live-reconnect.ppm')),indent=2)+'\n')
  print('Live reconnect controls',passed,'radio opens',starts);assert passed
 finally:
  if app.poll() is None:app.terminate();app.wait()
