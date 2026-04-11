import serial # need to "pip install pyserial" for this to work
import sliplib # need to "pip install sliplib" for this to work
import struct
from time import sleep


# Pad a string or bytes object out to a multiple of 4 octets
def OSCpad(s):
    if isinstance(s,str): # needs a \0 terminator
        r = bytes(s.encode('ascii'))+b'\x00'+b'\x00'*((-1-len(s))%4)
    else: # doesn't need a terminator
        r = s + b'\x00'*((-len(s))%4)
    #r = bytes(b'\x00'+b'\x00'*((-1-len(s))%4))
    #print(r)
    return r


# Pack an address, parameter types and parameters into an OSC message
def OSCpack(addr,pt,*argv):
    addr = OSCpad(addr)
    ptp = OSCpad(','+pt)
    fmt = '>' + str(len(addr))+'s' + str(len(ptp))+'s'
    rv = bytes(struct.pack(fmt,addr,ptp))
    for i in range(len(argv)):
        rv += bytes(struct.pack('>'+pt[i],argv[i]))
    #print(rv)
    return rv

 

# Pack an address and parameters into an OSC message
# The types of the parameters are determined automatically
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
    #print(rv)
    return rv   


# Make an OSC bundle for execution at time ttag, from the
# elements in ell. If any elements are themselves bundles, it
# is assumed that their timetags are >= ttag
# The default timetag is 1, which means "immediately"
def OSCmakeBundle(ell,ttag=1):
    rv = OSCpad(b'#bundle') + struct.pack(">Q",ttag)
    for el in ell:
        rv += struct.pack(">L%ds" % len(el),len(el),el)
    #print(rv)
    return rv
    
    
#########################################################################
# Set up communication channel
ser = serial.Serial('COM5',115200,timeout=1)
SLIPser = sliplib.SlipStream(ser,chunk_size=1)
#########################################################################                

# Create a bundle that we could get actioned immediately
msgl = []
msgl += [OSCpackAuto('/teensy1/dynamic/clearAll')]
msgl += [OSCpackAuto('/teensy1/dynamic/crOb','AudioOutputI2S','i2s')]
msgl += [OSCpackAuto('/teensy1/dynamic/crOb','AudioControlSGTL5000','sgtl5000')]
msgl += [OSCpackAuto('/teensy1/dynamic/crGrp','/','voice1')]
msgl += [OSCpackAuto('/teensy1/dynamic/crGrp','/voice1','i3')]
msgl += [OSCpackAuto('/teensy1/dynamic/crGrp','/voice1','i2')]
msgl += [OSCpackAuto('/teensy1/dynamic/crGrp','/voice1','i1')]
msgl += [OSCpackAuto('/teensy1/dynamic/crGrp','/voice1','i0')]
msgl += [OSCpackAuto('/teensy1/dynamic/crOb','AudioSynthWaveform','wav','/voice1/i*')]
msgl += [OSCpackAuto('/teensy1/dynamic/crOb','AudioSynthWaveform','wav','/voice1/i*')]
msgl += [OSCpackAuto('/teensy1/dynamic/crOb','AudioSynthWaveform','wav2','/voice1/i*')]
msgl += [OSCpackAuto('/teensy1/audio/sgtl5000/ena')]
msgl += [OSCpackAuto('/teensy1/audio/sgtl5000/vol',0.1)]
pkt = OSCmakeBundle(msgl)
print(pkt)
#for msg in msgl:
#    SLIPser.send_msg(msg)
#    print(SLIPser.recv_msg(),end='\n\n')

SLIPser.send_msg(pkt)
print(SLIPser.recv_msg(),end='\n\n')
sleep(0.5)



ser.close()
