import subprocess, time, ctypes, ctypes.wintypes as wt, os, glob, socket
u=ctypes.windll.user32; D=0x2; U=0x4
WND=ctypes.WINFUNCTYPE(wt.BOOL,wt.HWND,wt.LPARAM)
def click(x,y):
    u.SetCursorPos(x,y); time.sleep(.15); u.mouse_event(D,0,0,0,0); time.sleep(.08); u.mouse_event(U,0,0,0,0)
player=r'F:\dlss5-video\dlss5-video-amd\build-ninja\DLSSVideoPlayer.exe'
media=r'F:\dlss5-video\dlss5-video-amd\run-fg\realclip-5min.mkv'
p=subprocess.Popen([player,media],cwd=os.path.dirname(player))
hwnd=None
try:
    for _ in range(25):
        time.sleep(1); out=[]
        def cb(h,l):
            if not u.IsWindowVisible(h): return True
            n=u.GetWindowTextLengthW(h)
            if not n: return True
            b=ctypes.create_unicode_buffer(n+1); u.GetWindowTextW(h,b,n+1)
            if b.value=='DLSS 5 Video Player': out.append(h)
            return True
        u.EnumWindows(WND(cb),0)
        if out: hwnd=out[0]; break
    if not hwnd: print('NO_WINDOW'); raise SystemExit(1)
    u.MoveWindow(hwnd,40,40,2400,1320,True); time.sleep(.4)
    u.ShowWindow(hwnd,9); time.sleep(.2); u.SetForegroundWindow(hwnd); time.sleep(.6)
    click(490,1266); time.sleep(10); click(570,1144); time.sleep(8)
    d=r'C:\Users\ADMINI~1\AppData\Local\Temp\dlss5-cast-hls'
    print('DIR_EXISTS', os.path.isdir(d))
    print('DIR_FILES', [os.path.basename(x) for x in glob.glob(d+r'\*')][:20])
    for path in ['/live.m3u8']:
        s=socket.create_connection(('192.168.1.31',8080),timeout=5)
        s.sendall(('GET '+path+' HTTP/1.1\r\nHost: 192.168.1.31:8080\r\nConnection: close\r\n\r\n').encode())
        buf=b''
        while True:
            try: c=s.recv(4096)
            except socket.timeout: break
            if not c: break
            buf+=c
            if len(buf)>8192: break
        s.close()
        print('RAW_RESPONSE', repr(buf[:600]))
finally:
    if hwnd: u.PostMessageW(hwnd,0x10,0,0); time.sleep(1)
    if p.poll() is None: p.kill()
