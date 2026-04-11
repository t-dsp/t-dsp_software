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

pkt = OSCpackAuto('/teensy1/dynamic/de','pc1')                  
SLIPser.send_msg(pkt)
sleep(0.5)

pkt = OSCpackAuto('/teensy1/dynamic/de','wave?form2')                  
SLIPser.send_msg(pkt)

pkt = OSCpackAuto('/teensy1/dynamic/crOb','AudioSynthWaveform','[wave_form2]')                  
SLIPser.send_msg(pkt)

pkt = OSCpackAuto('/teensy1/audio/wave_form2/begin',0.2,330.0,0)                  
SLIPser.send_msg(pkt)

pkt = OSCpackAuto('/teensy1/dynamic/crCo','pc1')                  
SLIPser.send_msg(pkt)

pkt = OSCpackAuto('/teensy1/audio/pc1/connect','wave?form2',0,'mixer1',3)                  
SLIPser.send_msg(pkt)

ser.close()
