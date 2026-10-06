import serial, sys, time
port = sys.argv[1] if len(sys.argv) > 1 else '/dev/cu.usbmodem101'
dur = float(sys.argv[2]) if len(sys.argv) > 2 else 8
s = serial.Serial()
s.port = port; s.baudrate = 115200; s.timeout = 0.2
s.dtr = False; s.rts = False
s.open()
end = time.time() + dur
while time.time() < end:
    d = s.read(4096)
    if d: sys.stdout.write(d.decode('utf-8', 'replace')); sys.stdout.flush()
