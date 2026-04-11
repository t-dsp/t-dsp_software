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

def sendMessages(msgl):    
    pkt = OSCmakeBundle(msgl)
    print(pkt)

    SLIPser.send_msg(pkt)
    print(SLIPser.recv_msg(),end='\n\n')
    #sleep(0.5)
    
#########################################################################
# Set up communication channel
ser = serial.Serial('COM5',115200,timeout=1)
SLIPser = sliplib.SlipStream(ser,chunk_size=1)
#########################################################################                
poly = 4
# Create a bundle that we could get actioned immediately
msgl = []
# start with a blank slate
msgl += [OSCpackAuto('/teensy1/dynamic/clearAll')]

# adjust this to suit your audio system
msgl += [OSCpackAuto('/teensy1/dynamic/crOb','AudioControlSGTL5000','sgtl5000')]
msgl += [OSCpackAuto('/teensy1/dynamic/crOb','AudioOutputI2S','i2s')]

if 1:
    # create the root-level objects
    msgl += [OSCpackAuto('/teensy1/dynamic/crOb','AudioMixer4','mixer')]

    # create group and sub-groups
    msgl += [OSCpackAuto('/teensy1/dynamic/crGrp','voice1','/')]
    for i in range(0,poly):
        msgl += [OSCpackAuto('/teensy1/dynamic/crGrp','i%d'%i,'/voice1')]

    # create the audio objects for the four voice1 instances:
    msgl += [OSCpackAuto('/teensy1/dynamic/crOb','AudioMixer4','mixer','/voice1/i*')]
    msgl += [OSCpackAuto('/teensy1/dynamic/crOb','AudioEffectEnvelope','env','/voice1/i*')]
    msgl += [OSCpackAuto('/teensy1/dynamic/crOb','AudioSynthWaveform','wav2','/voice1/i*')]
    msgl += [OSCpackAuto('/teensy1/dynamic/crOb','AudioSynthWaveform','wav','/voice1/i*')]

    # now the connections
    msgl += [OSCpackAuto('/teensy1/dynamic/crCo','wav_mix0','/voice1/i*')]
    msgl += [OSCpackAuto('/teensy1/dynamic/crCo','wav2_mix1','/voice1/i*')]
    msgl += [OSCpackAuto('/teensy1/dynamic/crCo','mix_env','/voice1/i*')]
    #sendMessages(msgl)
    msgl += [OSCpackAuto('/teensy1/dynamic/crCo','env_mix','/voice1/i*')]
    
    for i in range(0,poly): # connect up the four instances of voice1:
        msgl += [OSCpackAuto('/teensy1/audio/voice1/i%d/wav_mix0/co' % i,'/voice1/i%d/wav'%i,'/voice1/i%d/mixer'%i)]
        msgl += [OSCpackAuto('/teensy1/audio/voice1/i%d/wav2_mix1/co' % i,'/voice1/i%d/wav2'%i,0,'/voice1/i%d/mixer'%i,1)]
        msgl += [OSCpackAuto('/teensy1/audio/voice1/i%d/mix_env/co' % i,'/voice1/i%d/mixer'%i,'/voice1/i%d/env'%i)]
        msgl += [OSCpackAuto('/teensy1/audio/voice1/i%d/env_mix/co' % i,'/voice1/i%d/env'%i,0,'/mixer',i)] # note, routes out of the instances!

    # set mixer gains to something vaguely sane   
    for i in range(0,poly):
        msgl += [OSCpackAuto('/teensy1/audio/mixer/ga',i,0.25)]
        msgl += [OSCpackAuto('/teensy1/audio/voice1/i*/mixer/ga',i,0.5)]
    #sendMessages(msgl)

    # root-level connections   
    msgl += [OSCpackAuto('/teensy1/dynamic/crCo','p_mix_outL')]
    msgl += [OSCpackAuto('/teensy1/dynamic/crCo','p_mix_outR')]
    msgl += [OSCpackAuto('/teensy1/audio/p_mix_outL/co','/mixer',0,'/i2s',0)]
    msgl += [OSCpackAuto('/teensy1/audio/p_mix_outR/co','/mixer',0,'/i2s',1)]
    
# extra setup
#msgl += [OSCpackAuto('/teensy1/audio/vo*/i*/env/release',20.0)]

# enable audio system    
msgl += [OSCpackAuto('/teensy1/audio/sgtl5000/ena')]
msgl += [OSCpackAuto('/teensy1/audio/sgtl5000/vol*',0.5)]  # currently not working without the *
msgl += [OSCpackAuto('/teensy1/audio/sgtl5000/lineO*',12)] # ~3Vpk-pk


pkt = OSCmakeBundle(msgl)
print(pkt)
SLIPser.send_msg(pkt)
print(SLIPser.recv_msg(),end='\n\n')
sleep(0.5)
############ Creation of system is done #####################

# test some things:
if 0:
    msgl  = [OSCpackAuto('/teensy1/dynamic/rename','mixer', 'moxer')]
    msgl += [OSCpackAuto('/teensy1/dynamic/rename','/vo*/i0/mixer', 'maxer')]
    msgl += [OSCpackAuto('/teensy1/dynamic/rename','/vo*/i*/mixer', 'maxer')]
    sendMessages(msgl)
    sleep(0.5)

# test some more things:
if 0:
    msgl  = [OSCpackAuto('/teensy1/dynamic/delete','moxer')]
    msgl += [OSCpackAuto('/teensy1/dynamic/delete','/vo*/i*/mixer')]
    pkt = OSCmakeBundle(msgl)
    print(pkt)

    SLIPser.send_msg(pkt)
    print(SLIPser.recv_msg(),end='\n\n')
    sleep(0.5)

# test even more things:
if 0:
    msgl  = [OSCpackAuto('/teensy1/dynamic/delete','/vo*/i0')]
    msgl += [OSCpackAuto('/teensy1/audio/voice1/i2/wav2_mix1/co','/voice1/i2/wav2',0,'/voice1/i2/lixer',2)]
    pkt = OSCmakeBundle(msgl)
    print(pkt)

    SLIPser.send_msg(pkt)
    print(SLIPser.recv_msg(),end='\n\n')
    sleep(0.5)

ser.close()
