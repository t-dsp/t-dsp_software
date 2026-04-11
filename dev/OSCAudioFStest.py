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
msgl += [OSCpackAuto('/teensy1/audio/waveform1/begi',0.1,220.0,11)]
msgl += [OSCpackAuto('/teensy1/audio/sgtl5000/ena')]
msgl += [OSCpackAuto('/teensy1/audio/sgtl5000/vol',0.1)]
pkt = OSCmakeBundle(msgl)

# save it to a file on Teensy instead
pkt1 = OSCpackAuto('/teensy1/fs/save','test1.osc',pkt)                  
#SLIPser.send_msg(pkt1)
#print(SLIPser.recv_msg(),end='\n\n')
#sleep(0.5)

# re-create sacrificial file
pkt3 = OSCpackAuto('/teensy1/fs/save','test3.osc',pkt)                  
SLIPser.send_msg(pkt3)
print(SLIPser.recv_msg(),end='\n\n')

#pkt = OSCpackAuto('/teensy1/fs/save','test.txt',b'This is a long piece of data which is intended to be copied to the filesystem in pieces, because it\'s too long to fit in the buffer all at once')                  
#SLIPser.send_msg(pkt)
#print(SLIPser.recv_msg(),end='\n\n')
#sleep(0.5)

# retrieve a file from the Teensy as a bundle in the reply
pkt = OSCpackAuto('/teensy1/fs/send','test1.osc') 
SLIPser.send_msg(pkt)
print(SLIPser.recv_msg(),end='\n\n')

# retrieve a file from the Teensy as a bundle in the reply
pkt = OSCpackAuto('/teensy1/fs/send','test2.osc') 
SLIPser.send_msg(pkt)
print(SLIPser.recv_msg(),end='\n\n')

# retrieve a file from the Teensy as a bundle in the reply
pkt = OSCpackAuto('/teensy1/fs/send','test3.osc') 
SLIPser.send_msg(pkt)
print(SLIPser.recv_msg(),end='\n\n')

# delete a file from the Teensy
pkt = OSCpackAuto('/teensy1/fs/delete','test3.osc') 
SLIPser.send_msg(pkt)
print(SLIPser.recv_msg(),end='\n\n')

# delete it again: probably fails
SLIPser.send_msg(pkt)
print(SLIPser.recv_msg(),end='\n\n')

# retrieve a file from the Teensy: should fail
pkt = OSCpackAuto('/teensy1/fs/send','test3.osc') 
SLIPser.send_msg(pkt)
print(SLIPser.recv_msg(),end='\n\n')
sleep(0.5)



# tell the Teensy to interpret a bundle from a file
pkt = OSCpackAuto('/teensy1/fs/load','test1.osc') 
SLIPser.send_msg(pkt)
print(SLIPser.recv_msg(),end='\n\n')
sleep(0.5)





ser.close()
