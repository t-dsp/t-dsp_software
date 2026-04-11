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

pkt = OSCpackAuto('/teensy1/dynamic/clearAll')                  
SLIPser.send_msg(pkt)
print(SLIPser.recv_msg(),end='\n\n')
sleep(0.5)

pkt = OSCpackAuto('/teensy1/dynamic/crOb','AudioControlSGTL5000','sgtl5000')                  
SLIPser.send_msg(pkt)
print(SLIPser.recv_msg(),end='\n\n')
sleep(0.5)

pkt = OSCpackAuto('/teensy1/dynamic/crOb','AudioOutputI2S','i2s')
SLIPser.send_msg(pkt)
print(SLIPser.recv_msg(),end='\n\n')
sleep(0.5)

pkt = OSCpackAuto('/teensy1/dynamic/crOb','AudioSynthWaveform','waveform1')
SLIPser.send_msg(pkt)
print(SLIPser.recv_msg(),end='\n\n')
sleep(0.5)

pkt = OSCpackAuto('/teensy1/dynamic/crCo','pc1')
SLIPser.send_msg(pkt)
print(SLIPser.recv_msg(),end='\n\n')
sleep(0.5)

pkt = OSCpackAuto('/teensy1/dynamic/crCo','pc1')
SLIPser.send_msg(pkt)
print(SLIPser.recv_msg(),end='\n\n')
sleep(0.5)

pkt = OSCpackAuto('/teensy1/audio/pc1/conn','waveform1','i2s')
SLIPser.send_msg(pkt)
print(SLIPser.recv_msg(),end='\n\n')
sleep(0.5)

pkt = OSCpackAuto('/teensy1/audio/pc1/dis')
SLIPser.send_msg(pkt)
print(SLIPser.recv_msg(),end='\n\n')
sleep(0.5)

pkt = OSCpackAuto('/teensy1/audio/pc1/conn','waveform1',0,'i2s',1)
SLIPser.send_msg(pkt)
print(SLIPser.recv_msg(),end='\n\n')
sleep(0.5)

pkt = OSCpackAuto('/teensy1/audio/pc1/dis')
#SLIPser.send_msg(pkt)
#print(SLIPser.recv_msg(),end='\n\n')
#sleep(0.5)



pkt = OSCpackAuto('/teensy1/audio/sgtl5000/enabl')                  
SLIPser.send_msg(pkt)
print(SLIPser.recv_msg(),end='\n\n')
sleep(0.5)

ser.close()
