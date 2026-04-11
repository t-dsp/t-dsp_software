import serial # need to "pip install pyserial" for this to work
import sliplib # need to "pip install sliplib" for this to work
import struct
from time import sleep

def OSCpad(s):
    if isinstance(s,str):
        r = bytes(s.encode('ascii'))+b'\x00'+b'\x00'*((-1-len(s))%4)
    else:
        r = s+b'\x00'+b'\x00'*((-1-len(s))%4)
    #r = bytes(b'\x00'+b'\x00'*((-1-len(s))%4))
    #print(r)
    return r


def OSCpack(addr,pt,*argv):
    addr = OSCpad(addr)
    ptp = OSCpad(','+pt)
    fmt = '>' + str(len(addr))+'s' + str(len(ptp))+'s'
    rv = bytes(struct.pack(fmt,addr,ptp))
    for i in range(len(argv)):
        rv += bytes(struct.pack('>'+pt[i],argv[i]))
    #print(rv)
    return rv

 

def OSCpackAuto(addr,*argv):
    pt = ''
    rv = b''
    for v in argv:
        if isinstance(v,int):
            t = 'i'
        elif isinstance(v,float):
            t = 'f'
        elif isinstance(v,str):
            t = 's'
            v = OSCpad(v)
        else:
            t = 'b'
            v = struct.pack('>i',len(v)) + OSCpad(v)
        pt += t
        l=''
        if 's' == t or 'b' == t:
            l = str(len(v))
        if 'b' == t:
            t = 's'
        rv += bytes(struct.pack('>'+l+t,v))
        
    addr = OSCpad(addr)
    ptp = OSCpad(','+pt)
    fmt = '>' + str(len(addr))+'s' + str(len(ptp))+'s'
    rv = bytes(struct.pack(fmt,addr,ptp)) + rv
    print(rv)
    return rv   
                 
ser = serial.Serial('COM5',115200)
SLIPser = sliplib.SlipStream(ser)

pkt = OSCpackAuto('/teensy1/dynamic/reeeeeeeeeeenamerrrrr test!') # mark the start - does nothing!
SLIPser.send_msg(pkt)
pkt = OSCpackAuto('/teensy1/dynamic/rename','mixer1','MAXer2')  # simple - works                
SLIPser.send_msg(pkt)
pkt = OSCpackAuto('/teensy1/dynamic/rename','MAXer2','waveform1')  # fails, attempt to duplicate name                
SLIPser.send_msg(pkt)
pkt = OSCpackAuto('/teensy1/dynamic/rename','MAXer2','MAX?er2')  # sanitisation lets this work                
SLIPser.send_msg(pkt)
pkt = OSCpackAuto('/teensy1/dynamic/rename','MAX_er2',' #*,/?[]{}') # fails, result string is empty
SLIPser.send_msg(pkt)
pkt = OSCpackAuto('/teensy1/dynamic/rename','[MAX*er2]','*?mixer1{}') # sanitisation lets this work
SLIPser.send_msg(pkt)


ser.close()
