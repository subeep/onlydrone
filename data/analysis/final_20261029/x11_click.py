"""Send ordinary X11 pointer events to the tested window's client coordinates."""
import ctypes as C,time
L=C.CDLL('libX11.so.6')
L.XOpenDisplay.argtypes=[C.c_char_p];L.XOpenDisplay.restype=C.c_void_p
L.XDefaultRootWindow.argtypes=[C.c_void_p];L.XDefaultRootWindow.restype=C.c_ulong
class Motion(C.Structure):
 _fields_=[('type',C.c_int),('serial',C.c_ulong),('send_event',C.c_int),('display',C.c_void_p),('window',C.c_ulong),('root',C.c_ulong),('subwindow',C.c_ulong),('time',C.c_ulong),('x',C.c_int),('y',C.c_int),('x_root',C.c_int),('y_root',C.c_int),('state',C.c_uint),('is_hint',C.c_char),('same_screen',C.c_int)]
class Button(C.Structure):
 _fields_=Motion._fields_[:-2]+[('button',C.c_uint),('same_screen',C.c_int)]
class Event(C.Union):
 _fields_=[('motion',Motion),('button',Button),('pad',C.c_long*24)]
L.XSendEvent.argtypes=[C.c_void_p,C.c_ulong,C.c_int,C.c_long,C.POINTER(Event)]
L.XFlush.argtypes=[C.c_void_p];L.XCloseDisplay.argtypes=[C.c_void_p]
def click(wid,x,y):
 d=L.XOpenDisplay(None);assert d
 root=L.XDefaultRootWindow(d)
 e=Event();e.motion=Motion(type=6,display=d,window=wid,root=root,x=x,y=y,same_screen=1)
 L.XSendEvent(d,wid,0,1<<6,C.byref(e));L.XFlush(d);time.sleep(.12)
 for typ,mask in [(4,1<<2),(5,1<<3)]:
  e=Event();e.button=Button(type=typ,display=d,window=wid,root=root,x=x,y=y,button=1,same_screen=1)
  L.XSendEvent(d,wid,0,mask,C.byref(e));L.XFlush(d);time.sleep(.12)
 L.XCloseDisplay(d)
