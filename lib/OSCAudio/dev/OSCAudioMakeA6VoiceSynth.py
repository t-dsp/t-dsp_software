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
# Requires AudioMixerX aka DynMixer
poly = 6
pt8211 = False
i2s = True
dac = False
# Create a bundle that we could get actioned immediately
msgl = []
# start with a blank slate
msgl += [OSCpackAuto('/teensy1/dynamic/clearAll')]

# adjust this to suit your audio system
if pt8211:
    msgl += [OSCpackAuto('/teensy1/dynamic/crOb','AudioOutputPT8211','pt8211')]
if i2s:
    msgl += [OSCpackAuto('/teensy1/dynamic/crOb','AudioControlSGTL5000','sgtl5000')]
    msgl += [OSCpackAuto('/teensy1/dynamic/crOb','AudioOutputI2S','i2s')]
if dac:
    msgl += [OSCpackAuto('/teensy1/dynamic/crOb','AudioOutputAnalogStereo','dac')]

if 1:
    # create the root-level objects
    msgl += [OSCpackAuto('/teensy1/dynamic/crOb','AudioMixerStereo','mixer',poly)]

    # create group and sub-groups
    msgl += [OSCpackAuto('/teensy1/dynamic/crGrp','voice1','/')]
    for i in range(0,poly):
        msgl += [OSCpackAuto('/teensy1/dynamic/crGrp','i%d'%(poly-1-i),'/voice1')]

    # create the audio objects for the four voice1 instances:
    msgl += [OSCpackAuto('/teensy1/dynamic/crOb','AudioMixer4','mixer','/voice1/i*')]
    msgl += [OSCpackAuto('/teensy1/dynamic/crOb','AudioEffectEnvelope','env','/voice1/i*')]
    msgl += [OSCpackAuto('/teensy1/dynamic/crOb','AudioSynthWaveform','wav2','/voice1/i*')]
    msgl += [OSCpackAuto('/teensy1/dynamic/crOb','AudioSynthWaveform','wav','/voice1/i*')]

    # now the connections
    msgl += [OSCpackAuto('/teensy1/dynamic/crCo','wav_mix0','/voice1/i*')]
    msgl += [OSCpackAuto('/teensy1/dynamic/crCo','wav2_mix1','/voice1/i*')]
    msgl += [OSCpackAuto('/teensy1/dynamic/crCo','mix_env','/voice1/i*')]
    msgl += [OSCpackAuto('/teensy1/dynamic/crCo','env_mix','/voice1/i*')]
    
    for i in range(0,poly): # connect up the instances of voice1:
        msgl += [OSCpackAuto('/teensy1/audio/voice1/i%d/wav_mix0/co' % i,'/voice1/i%d/wav'%i,'/voice1/i%d/mixer'%i)]
        msgl += [OSCpackAuto('/teensy1/audio/voice1/i%d/wav2_mix1/co' % i,'/voice1/i%d/wav2'%i,0,'/voice1/i%d/mixer'%i,1)]
        msgl += [OSCpackAuto('/teensy1/audio/voice1/i%d/mix_env/co' % i,'/voice1/i%d/mixer'%i,'/voice1/i%d/env'%i)]
        msgl += [OSCpackAuto('/teensy1/audio/voice1/i%d/env_mix/co' % i,'/voice1/i%d/env'%i,0,'/mixer',i)] # note, routes out of the instances!

    # set mixer gains to something vaguely sane   
    for i in range(0,poly):
        msgl += [OSCpackAuto('/teensy1/audio/mixer/gain',i,0.16)]
        msgl += [OSCpackAuto('/teensy1/audio/mixer/pan',i,(2.0*float(i)/(poly - 1)-1.0)*0.9)]
        msgl += [OSCpackAuto('/teensy1/audio/voice1/i*/mixer/ga',i,0.5)]

    # root-level connections
    if pt8211:
        msgl += [OSCpackAuto('/teensy1/dynamic/crCo','p_mix_outL')]
        msgl += [OSCpackAuto('/teensy1/dynamic/crCo','p_mix_outR')]
        msgl += [OSCpackAuto('/teensy1/audio/p_mix_outL/co','/mixer',0,'/pt8211',0)]
        msgl += [OSCpackAuto('/teensy1/audio/p_mix_outR/co','/mixer',1,'/pt8211',1)]
      
    if i2s:
        msgl += [OSCpackAuto('/teensy1/dynamic/crCo','p_mix_outL')]
        msgl += [OSCpackAuto('/teensy1/dynamic/crCo','p_mix_outR')]
        msgl += [OSCpackAuto('/teensy1/audio/p_mix_outL/co','/mixer',0,'/i2s',0)]
        msgl += [OSCpackAuto('/teensy1/audio/p_mix_outR/co','/mixer',1,'/i2s',1)]

    if dac:
        msgl += [OSCpackAuto('/teensy1/dynamic/crCo','p_mix_dacL')]
        msgl += [OSCpackAuto('/teensy1/dynamic/crCo','p_mix_dacR')]
        msgl += [OSCpackAuto('/teensy1/audio/p_mix_dacL/co','/mixer',0,'/dac',0)]
        msgl += [OSCpackAuto('/teensy1/audio/p_mix_dacR/co','/mixer',1,'/dac',1)]

# enable audio system
if i2s:
    msgl += [OSCpackAuto('/teensy1/audio/sgtl5000/enab')]
    msgl += [OSCpackAuto('/teensy1/audio/sgtl5000/vo',0.5)]
    msgl += [OSCpackAuto('/teensy1/audio/sgtl5000/lineO*',12)] # ~3Vpk-pk


pkt = OSCmakeBundle(msgl)
print(pkt)
#for msg in msgl:
#    SLIPser.send_msg(msg)
#    print(SLIPser.recv_msg(),end='\n\n')

SLIPser.send_msg(pkt)
rpl = SLIPser.recv_msg()
print(rpl,end='\n\n')
sleep(0.5)



ser.close()
