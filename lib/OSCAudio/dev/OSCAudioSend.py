import serial # need to "pip install pyserial" for this to work
import sliplib # need to "pip install sliplib" for this to work
import struct
from time import sleep

def OSCpad(s):
    if isinstance(s,str):
        r = bytes(s.encode('ascii'))+b'\x00'+b'\x00'*((-1-len(s))%4)
    else:
        r = s+b'\x00'*((-len(s))%4)
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
                 
#########################################################################
# Set up communication channel
ser = serial.Serial('COM5',115200,timeout=1)
SLIPser = sliplib.SlipStream(ser,chunk_size=1)
######################################################################### 

s1 = OSCpad('/teensy1/audio/waveform1/begin')
s2 = OSCpad(',ffi')
pkt = struct.pack('>' + str(len(s1))+'s' + str(len(s2))+'sffi',
                  s1,
                  s2,
                  0.8,220.0,11
                  #789.019775390625,(2.6588213443756104),0x41424344
                  )
SLIPser.send_msg(pkt)
print(pkt)
print(SLIPser.recv_msg())
#print(ser.read(200))
print()
sleep(0.5)

pkt = OSCpackAuto('/teensy1/audio/sgtl5000/enabl')                  
SLIPser.send_msg(pkt)
sleep(0.5)

pkt = OSCpackAuto('/teensy1/audio/sgtl5000/vol',0.2)                  
SLIPser.send_msg(pkt)
sleep(0.5)

pkt = OSCpackAuto('/teensy1/audio/sgtl5000/vol',0.1)                  
SLIPser.send_msg(pkt)
sleep(0.5)

pkt = OSCpackAuto('/teensy1/audio/waveform1/be',0)                  
SLIPser.send_msg(pkt)
sleep(0.5)

pkt = OSCpackAuto('/teensy1/audio/mixer1/ga',1,1.0)                  
SLIPser.send_msg(pkt)
sleep(0.5)

pkt = OSCpackAuto('/teensy1/dynamic/rena','mixer1','MAXer2')                  
SLIPser.send_msg(pkt)
pkt = OSCpackAuto('/teensy1/audio/MAXer2/ga',1,0.1)                  
SLIPser.send_msg(pkt)
sleep(0.5)

pkt = OSCpackAuto('/teensy1/dynamic/rena','MAXer2','mixer1')                  
SLIPser.send_msg(pkt)
pkt = OSCpackAuto('/teensy1/audio/waveform1/fr',440.0)                  
SLIPser.send_msg(pkt)
sleep(0.5)

pkt = OSCpackAuto('/teensy1/audio/waveform1/amplitude',0.1)                  
SLIPser.send_msg(pkt)
sleep(0.5)

pkt = OSCpackAuto('/teensy1/audio/waveform1/amp',0.0)                  
SLIPser.send_msg(pkt)

pkt = OSCpackAuto('/teensy1/audio/wave_form2/fr',550.0)                  
SLIPser.send_msg(pkt)
ser.close()
