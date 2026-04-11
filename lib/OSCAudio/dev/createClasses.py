import re
import fileinput
import os
import json

##############################################################################################
# User settings
dynamic = True        
ftrl = ['play_wav' # files to reject
        ]

limit = 1000
rp = '../../Audio'
idxf = '../../Audio/gui/index.html'
# USB audio is in cores, not the Audio library. At time of writing
# Teensy 3.x and 4.x have the same class structure.
ar = 'c:/Program Files (x86)/Arduino/hardware/teensy/avr/cores/teensy4/'
afl = ['usb_audio.h']

##############################################################################################
def printFn(base,fnT):
    print("%s,%s,%s,%s," % ((base,)+fnT))


def paramToType(p):
    result = None
    p = re.sub('\s*=.*$','',p);      # remove default value, if present
    pq = re.sub('\w+$','',p).strip() # remove word from end
    if '' != pq: # deal with type-only declaration
        p = pq
    if re.search('float',p):
        result = 'f'

    if re.search('(short|int|signed)',p):
        result = 'i'

    m = re.search('(AudioFilterLadderInterpolation|behaviour_e|AudioEffectDelayMemoryType_t)',p)
    if m:
        result = ('i',m.group(1)) # integer, but cast to enum

    if re.search('void',p):
        result = 'v'

    if re.search('bool',p):
        result = ';'

    # put these last: char* is string,
    # any other type* is blob
    if re.search('[*&]',p):
        result = 'b'

    if re.search('char\s+[*]',p):
        result = 's'

    return result

##############################################################################################
undefinedFns = { # member functions declared but not actually defined anywhere
     'AudioAnalyzePrint': ['.*trigger.*float.*int']
    ,'AudioPlayQueue': ['.*stop.*void']
    ,'AudioOutputSPDIF3': ['.*static.*pll_locked.*void']
    }
if not dynamic:
    undefinedFns['AudioAmplifier'] = ['.*void.*slew.*bool']


def alterPtypeFBQ(li,sf):
    result = ('iddddd','setC*')
    if re.search(r'int\s*[*]',li):
        result = ('iiiiii','setC*')
    return result
        
    
helperFns = { # member functions which need special "helpers"
    'AsyncAudioInputSPDIF3': [{'zapCon': 'y'}],
    'AudioControlTLV320AIC3206': [{'zapCon': 'y'}],
    'AudioAnalyzeFFT1024~windowFunction': ['s',';','bool',{'doc': 'string selecting one of the built-in window types, e.g. "Hanning"'}],
    'AudioAnalyzeFFT256~windowFunction': ['s',';','bool',{'doc': 'string selecting one of the built-in window types, e.g. "BlackmanHarris"'}],
    'AudioAnalyzePrint~name': ['s',';','bool',{'dtor': 'free(namePtr);','vars': ['char* namePtr']}],
    'AudioAnalyzePrint': [{'init': ', namePtr(0)'}],
    'AudioControlSGTL5000~eqFilter': ['ib','void','void',{'doc': 'filter number; blob holding 7 int32 parameters'}],
    'AudioControlTLV320AIC3206~setIIRCoeffOnADC': ['ib','void','void',{'doc': 'filter channel; blob holding 3 uint32 coefficients'}],
    'AudioEffectGranular~begin': ['bi','void','void',{'dtor': 'free(sample_bank);', 'vars': ['int16_t* sample_bank'],'doc': 'blob holding 16-bit grain data; number of samples in data'}],
    'AudioEffectGranular': [{'init': ', sample_bank(0)'}],
    'AudioFilterBiquad~setCoefficients': ['iddddd','void','void',{'ptype': alterPtypeFBQ,'doc': 'filter stage number, followed by 5 coefficient values'}],
    'AudioPlayMemory~play': ['i','void','void',{'doc': '32-bit integer pointing to valid data built in to sketch'}],
    'AudioPlaySdRaw~play': ['s','void','void'],
    'AudioPlaySdWav~play': ['s','void','void'],
    'AudioPlaySerialflashRaw~play': ['s','void','void'],
    'AudioSynthWaveform~arbitraryWaveform': ['bf',';','bool',{'dtor': 'free(arbdata);', 'init': ', arbdata(0)', 'vars': ['int16_t* arbdata'],'doc': 'blob containing 256 samples; float is currently unused'}],
    'AudioSynthWaveform': [{'init': ', arbdata(0)'}],
    'AudioSynthWaveformModulated~arbitraryWaveform': ['bf',';','bool',{'dtor': 'free(arbdata);', 'init': ', arbdata(0)','vars': ['int16_t* arbdata'],'doc': 'blob containing 256 samples; float is currently unused'}],
    'AudioSynthWaveformModulated': [{'init': ', arbdata(0)'}],
    }
getd = {
    'i': 'getInt(%d)',
    'f': 'getFloat(%d)',
    's': 'getString(%d)',
    'b': 'getBlob(%d)',
    ';': 'getBoolean(%d)',
    }
    
# return value cast dict
rtcd = {
    'boolean': 'bool',
    'envelopeStateEnum': 'uint8_t',
    'AudioEffectDelayMemoryType_t': 'int',
    'double': 'float',
    'int': 'int32_t',
    'int16_t *': 'uint32_t',
    'unsigned': 'uint32_t',
    'unsigned char': 'uint8_t',
    'unsigned int': 'uint32_t',
    'unsigned short': 'uint16_t'
}
notD = ['AudioAnalyzeEvent', 'AudioEffectExpEnvelope', 'AudioMixer', 'AudioMixerStereo'] # not in static library
notEver = ['AudioMixerBase', 'AudioControlI2C'] # not in either library
returnTypes={}

##############################################################################################
# Convert resource share setting to enum
# Could be a list
def settingToEnum(s):
    if isinstance(s,list):
        s = 'Or'.join(s)
    return s

##############################################################################################
# Convert dict entry to resource settings enums
def rsrcEnum(d):
    dr = {}
    if 'setting' in d: # shareable
        dr['setting'] = 'setg_' + settingToEnum(d['setting']).replace(' ','_')
    else:
        dr['setting'] = 'setgUnshareable'
    dr['resource'] = 'rsrc_' + d['resource'].replace(' ','_')

    return dr


##############################################################################################
def mkClass(cn,fd):
    cno = cn.replace('Audio','OSCAudio')
    init = ''
    if cn in helperFns and 'init' in helperFns[cn][0]:
        init = helperFns[cn][0]['init']
    s = ''
    tst = 'if' # first test for address match is "if" statement: rest will be "else if"
    pfd={}
    varl = []
    ctorBody = ''
    dtorBody = ''
    for fi in sorted(fd): # go through public functions
        if '~' not in fi: # class-global stuff, not a function
            continue
        fn = fd[fi]['f'] # function name
        sf = fd[fi]['s'] # shortened function name
        li = fd[fi]['l'] # raw line from original declaration
        rt = fd[fi]['r'] # return type

        rt = rt.replace('friend','').replace('static','').strip()
        rtc= ''
        if rt in rtcd:
            rtc = '(' + rtcd[rt] + ')'
        pt = ''
        pl = ''
        cmnt = ''
        for i in range(len(fd[fi]['p'])): # go through parameter list
            cast = ''
            p = fd[fi]['p'][i]
            t = paramToType(p)
            if isinstance(t,tuple):
                cast = '(' + t[1] + ') '
                t = t[0]
            if t and t != 'v':
                pt += t
                pl += F',{cast}msg.' + (getd[t] % i)
            if t and (t == 'b' or t == 's'):
                cmnt = '// ' # comment out functions needing blobs and strings, for now

            # Special case rejection of functions declared
            # but not defined etc.
            if cn in undefinedFns:
                for rex in undefinedFns[cn]:
                    if re.search(rex,li):
                        cmnt = '// NOT DEFINED: '

        # Special cases for methods that need a "helper function"
        # to work properly
        hfi = cn+'~'+fn
        if hfi in helperFns:
            pt,rt = helperFns[hfi][:2]
            pl = '~msg'
            cmnt = ''
            pfd[fn] = helperFns[hfi]
            if len(helperFns[hfi]) > 3:
                hfd = helperFns[hfi][3]
                if 'dtor' in hfd:
                    dtorBody = ' '.join((dtorBody,hfd['dtor']))
                if 'vars' in hfd:
                    varl += hfd['vars']
                if 'ptype' in hfd:
                    pt,sf = hfd['ptype'](li,sf)
                if 'doc' in hfd:
                    fd[fi]['doc'] = hfd['doc']

        if len(pt) > 0:
            pt = '"'+pt+'"'
        else:
            pt = 'NULL'

        returnTypes[rt] = 'y'
        fd[fi]['o'] = pt
        if '' != cmnt:
            fd[fi]['exclude'] = 'y'
        if 'void' == rt:
            s += f'            {cmnt}{tst} (isTarget(msg,addrOff,"/{sf}",{pt})) {{{fn}({pl[1:]}); addReplyExecuted(msg,addrOff,reply,nameOfTarget);}} // {li}\n'
        else:
            s += f'            {cmnt}{tst} (isTarget(msg,addrOff,"/{sf}",{pt})) {{addReplyResult(msg,addrOff,reply,{rtc}{fn}({pl[1:]}),nameOfTarget); }} // {li}\n'
        if '' == cmnt:
            tst = 'else if'
            
    # ours, but no method matched: say so
    final = 'else '
    if 'if' == tst:
        final = ''
    s += f'            {final}addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);\n'
    
    # now go though any private helper functions we may need
    s += '''          }\n\t\t}\n'''
    if len(pfd) > 0:
        s += '\tprivate:\n'
        for fn in pfd:
            s += f'\t\t{pfd[fn][2]} {fn}(OSCMessage& msg);\n'
    if len(varl) > 0:
        for vdec in varl:
            s += f'\t\t{vdec};\n'

    # deal with constructor parameters
    if cn in helperFns and 'zapCon' in helperFns[cn][0]: # insane constructors!
        fd['cp'] = ''
    cp = ''
    cinit = ''
    oscpt = ''
    if 'cp' in fd and '' != fd['cp']:
        spl=fd['cp'].split(',')
        ospl = []
        cinit = F'{cn}('
        for spo in spl:
            cast = ''
            sp = re.sub('=[^,]*','',spo) # remove parameter defaults
            t = paramToType(sp)
            oscpt += t[0]  # works for string or tuple
            sp = sp.split(' ')
            if isinstance(t,tuple):
                print(t[1],spo)
                cast = f"({t[1]}) "
                spo = spo.replace(t[1],"int")  # sleazy hack for now
            cinit = cinit + F'{cast}{sp[-1]}, '
            ospl.append(spo)
        cinit = cinit[:-2] + '), ' + F'/* {oscpt} */ '
        fd['oscpt'] = oscpt
        cp = ', '+(", ".join(ospl))  #+fd['cp'] # append constructor parameters to derived class 
        
    # resource checking
    if dynamic and 'excl' in fd:
        rsrcCheck = '\n        '
        rcnt = len(fd['excl'])
        rsrcCheck += F'const static OSCAudioResourceCheck_t resources[{rcnt}];\n'
        rsrcCheck += F'        static rsrcState_e rsrcState;\n'
        
        ctorBody += ' rsrcState = rsrcThisActive;'
        dtorBody += ' rsrcState = rsrcThisDormant;'
        
        rsrcArrayValue = '\n#if defined(OSC_RSRC_ENABLE_DEFINE_ARRAYS)'
        rsrcArrayValue += F'\nconst OSCAudioResourceCheck_t {cno}::resources[] = {{\n'
        for rsrc in fd['excl']:
            rd = rsrcEnum(rsrc)
            rsrcArrayValue += F"  {{{rd['resource']},{rd['setting']}}},\n"
        rsrcArrayValue += '};\n'            
        rsrcArrayValue += F'rsrcState_e {cno}::rsrcState;\n'            
        rsrcArrayValue += '#endif // defined(OSC_RSRC_ENABLE_DEFINE_ARRAYS)\n'
    else:
        rsrcCheck = ''
        rsrcArrayValue = ''
        
    
    # start of class definition
    dtorLine = ''
    if '' != dtorBody:
        dtorLine = F'''        ~{cno}() {{{dtorBody}}} \n'''

    if dynamic: # need destructor and resource check for dynamic library
        s = F'''class {cno} : public {cn}, public OSCAudioBase
{{
    public:
        {cno}(const char* _name{cp}) : {cinit} OSCAudioBase(_name, (AudioStream*) this){init} {{{ctorBody}}}
        {cno}(const char* _name, OSCAudioGroup& grp{cp}) : {cinit} OSCAudioBase(_name, grp, (AudioStream*) this){init} {{{ctorBody}}}
{dtorLine}{rsrcCheck}
        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {{
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {{
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
''' + s
    else: # no destructor, resource check etc.
        s = F'''class {cno} : public {cn}, public OSCAudioBase
{{
    public:
        {cno}(const char* _name{cp}) : {cinit} OSCAudioBase(_name, (AudioStream*) this){init} {{}}
        {cno}(const char* _name, OSCAudioGroup& grp{cp}) : {cinit} OSCAudioBase(_name, grp, (AudioStream*) this){init} {{{ctorBody}}}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {{
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {{
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
''' + s
    
    # end of class definition
    s += F'''}};{rsrcArrayValue}
'''

        
        
    return s


##############################################################################################
def mkShort(d):
    ac = 1
    while True:
        # make a list of shortened function names
        # of length al, for every function which doesn't
        # already have a shortened name
        shrtd={}
        fnd = {}
        for fn in d:
            if '~' not in fn:
                continue
            if 's' not in d[fn]:
                shrt = d[fn]['f'][:ac]
                kt = d[fn]['f']
                if shrt not in shrtd:
                    shrtd[shrt] = [kt]
                    fnd[shrt] = [fn]
                else:
                    fnd[shrt] += [fn]
                    if kt not in shrtd[shrt]:
                        shrtd[shrt] += [kt]
        # find all shortened names that match just one function
        for shrt in list(shrtd):
            if len(shrtd[shrt]) == 1: # unique short version
                for fn in fnd[shrt]:
                    s = shrt
                    if len(d[fn]['f']) > ac: # is actually shorter!
                        s += '*'
                    d[fn]['s'] = s
                del shrtd[shrt]
        if len(shrtd) == 0:
            break
        ac += 1


##############################################################################################

d={}

# scan the .h files looking for classes derived from AudioStream or AudioControl 
#for root,dirs,files in os.walk(rp):
def processFiles(root,dirs,files):
    global limit,d
    if limit < 0:
        return
    for fn in files:
        m = re.search('(.*)[.]h$',fn)
        if m and m.group(1) not in ftrl:

            fp = os.path.join(root,fn)
            #print(fp)

            fi = fileinput.input(fp,openhook=fileinput.hook_encoded("utf-8"))
            cs = None
            base = None
            ndef = None
            conParams = ''
            for li in fi:
                #print(fi.filelineno())
                li = li.strip()

                m = re.search('^#ifndef *([^ ]*)',li)
                if m and not ndef:
                    ndef = m.group(1)

                if base:
                    m = re.search('^[^~:]*'+base+'[(]([^)]*)[)]',li)
                    if m and 'cp' not in d[base]:
                        cp = m.group(1).replace('void','').strip()
                        d[base]['cp'] = cp
                        if '' != cp:
                            print(li)
                                            
                m = re.search('class *([^ :]+) *:.*(Audio(?:Stream|Control|MixerBase)[^/{]*)',li)
                if m:
                    cs = 'private'
                    base = m.group(1)
                    d[base]={}
                    s = m.group(2).replace("public","").replace(" ","").split(",")
                    d[base]["baseClass"] = s
                    d[base]["li"] = li
                    if ndef: # expect guard before class definition starts!
                        d[base]['ndef'] = ndef
                    #print(base)

                m = re.search('(public|private|protected) *:',li)
                if cs and m:
                    cs = m.group(1)

                m=re.search(r'^\s*((?:virtual\s+|static\s+|unsigned\s+|friend\s+)*\w+(?:\s*[*])?)\s+([^(/[=]+)[(]([^)/]*)[)]',li)
                if m and 'public' == cs:
                    # groups are return type, function name, parameter list
                    output = True
                    if re.search(r'^Audio(In|Out)put',base) and \
                        'begin' == m.group(2): # audio I/O begin function
                        output = False

                    if re.search(r'^(usb_audio_)',m.group(2)): # USB internal function
                        output = False

                    if re.search(r'^(update|if\s)$',m.group(2)): # update function, if statement
                        output = False

                    if re.search(r'(isr)$',m.group(2)): # ISR function
                        output = False

                    if re.search(r'^(setMasterMode)$',m.group(2)): # non-existent function!
                        output = False

                    if re.search(r'^(return|else|operator)$',m.group(1)): # return or else
                        output = False

                    if output:
                        #printFn(base,m.groups())
                        p = list(m.group(3).split(','))
                        d[base][m.group(2)+'~'+m.group(3)] = {'f':m.group(2), 'r': m.group(1),
                                                              'p': p, 'l': li, 'o': fn}

            limit -= 1
            if limit < 0:
                break
            fi.close()

for root,dirs,files in os.walk(rp):
    processFiles(root,dirs,files)

processFiles(ar,[],afl)
print()

###############################################################################
# Scan the GUI looking for "InputOutputCompatibilityMetadata"
# These are cross-checked in the GUI's view.js checkRequirements() function
# at line 994.
# There is a conflict if a named resource is not shareable, or
# a shareable resource has different settings for two object types.
#
# At time of writing, we have:
#  34 object types which have compatibility criteria
#  21 resource types
#   9 resource settings
#
# Make an array of resource types which can be in a state of:
#   unused: can create an object that uses them
#   unusable: object is using them, resurce isn't shareable
#   setting made: in use, but can be shared if new object can co-exist with setting
fi = fileinput.input(idxf)
jstr=''
cap = False
for li in fi:
    if cap:
        jstr += li.strip()
        
    m = re.search(r'InputOutputCompatibilityMetadata',li)
    if m:
        cap = True
        #print ('==========================================================')
        continue
    
    m = re.search('^\s*]}$',li)
    if m:
        cap = False
        break
fi.close()
#print(jstr)        

js = json.loads(jstr)
js = js['requirements']

opd = {}
objd = {}
rsrcd = {}
sets = set()
for tdd in js: # one compatibility requirement...
    typ = '' # don't know the object type yet
    rss = {} # or the nature of the requirement
    for k in tdd:
        if k not in opd: # new key: type, shareable etc.
            opd[k] = set()
        toAdd = str(tdd[k]) #.replace(' ','_')
        opd[k].add(toAdd)

        if 'type' == k:
            typ = tdd[k]
        else:
            rss[k] = tdd[k]
            
    if typ not in objd:
        objd[typ] = [] # start compatibility list for this type
    objd[typ] += [rss] # add to this type's compatibility requirements
    
    rsrc = rss['resource']
    if rsrc not in rsrcd:
        rsrcd[rsrc] = set()
    if 'setting' in rss:
        setg = settingToEnum(rss['setting'])
        rsrcd[rsrc].add(setg)
        sets.add(str(setg))
        
for obj in sorted(objd):
    #print(obj)
    #print(objd[obj])
    if obj in d:
        d[obj]['excl'] = objd[obj]
###############################################################################
    

# Some special cases:
fi = fileinput.input(os.path.join(rp,'data_windows.c'),openhook=fileinput.hook_encoded("utf-8"))
FFTwd={'256': [], '1024': []}
for li in fi:
    m = re.search('AudioWindow(.+)(256|1024)',li)
    if m:
        FFTwd[m.group(2)] += [m.group(1)]
fi.close()

# Classes derived from AudioControlI2C should expose some of
# the base class functionality:
ACi2c = d["AudioControlI2C"]
ACi2cFns = {}
for k in ACi2c:
    if "~" in k:
        if "include-in-OSC" in ACi2c[k]["l"]:
            ACi2cFns[k] = ACi2c[k]
for cl in d:
    if "AudioControlI2C" in d[cl]["baseClass"]:
        for k in ACi2cFns:
            d[cl][k] = ACi2cFns[k]

# more special cases
if not dynamic:
    for cl in notD:
        try:
            del d[cl]
        except:
            pass
for cl in notEver:
    try:
        del d[cl]
    except:
        pass
    
#############################################################################################################
# Output file
op = r'..'  # r'E:\Jonathan\Arduino\libraries\OSCAudio'
of = 'OSCAudioAutogen.h'
ofjson = of.replace('.h','.json')
ofs = open(os.path.join(op,of),mode='w')

fguard = 'OSC_AUTOGEN_H'

# Requirements checking enums
# These are first because we need some conditional compilation magic
rguard = 'OSC_RSRC_TYPEDEF_ONLY'
stw = F'''

#if !defined({fguard})
#define {fguard}

'''

if dynamic:
    stw += F'''#if defined({rguard})
#undef {fguard}

#if !defined({rguard}_E)
#define {rguard}_E
'''

    stw += 'typedef enum {'
    for stg in ['setgAvailable','setgUnshareable']+list(map(lambda s:'setg_'+s.replace(' ','_'),sorted(sets))):
        stw += F'{stg}, '
    stw += 'setg_COUNT} resourceSetting_e;\n'
    ofs.write(stw)
    stw = 'typedef enum {'
    for stg in list(map(lambda s:'rsrc_'+s.replace(' ','_'),sorted(rsrcd))):
        stw += F'{stg}, '
    stw += F'''rsrc_COUNT}} resourceType_e;'''

    stw += F'''
#endif // !defined({rguard}_E)

#else // {rguard}

'''
ofs.write(stw)



# Classes
MACparams = 'a,o,c'
consTypes = set()
if not dynamic:
    MACparams = 'a'
    
for cl in sorted(d):
    ndef = ''
    if 'ndef' in d[cl]:
        ndef  = '#if defined(%s)\n' % d[cl]['ndef']
        ndefr = d[cl]['ndef']
    stw = F'{ndef}// ============== {cl} ===================='
    print(cl)
    
    mkShort(d[cl])
    stw += '\n' + mkClass(cl,d[cl])
    
    oscpt =''
    if 'oscpt' in d[cl]: # constructor parameters are discovered in mkClass()
        oscpt = d[cl]['oscpt']
        consTypes.add(oscpt)
    #print(stw)
    if ndef:
        stw += F'#define OSC_CLASS_{ndefr}({MACparams}) OSC_CLASS{oscpt}({MACparams})\n'
        if dynamic and 'excl' in d[cl]:
            stw += F'#define OSC_AUDIO_CHECK_RSRC_{ndefr}(c) OSC_AUDIO_CHECK_RSRC(c)\n'
        stw +=  '#else\n'
        stw += F'#define OSC_CLASS_{ndefr}({MACparams})\n'
        if dynamic and 'excl' in d[cl]:
            stw += F'#define OSC_AUDIO_CHECK_RSRC_{ndefr}(c)\n'
        stw +=  '#endif // defined(%s)\n\n' % d[cl]['ndef']
    else:
        stw += '\n'
    ofs.write(stw)


# dump database in JSON format
# here because mkClass() adds information about 
# constructor parameter types
ofj = open(os.path.join(op,ofjson),mode='w')
json.dump(d,ofj,indent=2)
ofj.close()


# List of classes: 
def writeEntry(ofs,cl):
    if 'ndef' in d[cl]:
        ndef = '_' + d[cl]['ndef']
    else:
        ndef = ''
        
    clo = cl.replace('Audio','OSCAudio')
    
    if 'excl' in d[cl]: # hardware requirement check?
        chk = clo
    else:
        chk = 'noRequirementCheck'
        
    if dynamic:
        ofs.write(f"\tOSC_CLASS{ndef}({cl},{clo},{chk}) \\\n")
    else:
        ofs.write(f"\tOSC_CLASS{ndef}({cl}) \\\n")

ofs.write('''// Need to define the following for complete coverage of OSC classes:
/*
#define OSC_CLASS(...)
''')
for sfx in consTypes:
    ofs.write(F'#define OSC_CLASS{sfx}(...)\n')
ofs.write('*/\n\n')

# void constructors only
ofs.write('#define OSC_AUDIO_CLASSES \\\n')
for cl in sorted(d):
    if 'cp' not in d[cl] or d[cl]['cp'] == '': 
        writeEntry(ofs,cl)
ofs.write("\n\n")

# non-void constructors
for sfx in consTypes:
    ofs.write(f'#define OSC_AUDIO_CLASSES{sfx} \\\n')
    for cl in sorted(d):
        if 'oscpt'in d[cl] and d[cl]['oscpt'] == sfx: 
            writeEntry(ofs,cl)
    ofs.write("\n\n")


# List of objects that need resource checks
if dynamic:
    ofs.write('#define OSC_AUDIO_CHECK_RSRC_LIST \\\n')
    chk = 'noRequirementCheck'
    ndef = chk
    ofs.write(f"\tOSC_AUDIO_CHECK_RSRC_{ndef}({chk}) \\\n")
    for cl in sorted(d):
        if 'excl' in d[cl] and ('cp' not in d[cl] or d[cl]['cp'] == ''):
            if 'ndef' in d[cl]:
                ndef = '_' + d[cl]['ndef']
            else:
                ndef = ''
            chk = cl.replace('Audio','OSCAudio')
            ofs.write(f"\tOSC_AUDIO_CHECK_RSRC{ndef}({chk}) \\\n")
    ofs.write("\n")



if dynamic:
    stw = F'''
#endif // defined({rguard})
#endif // defined({fguard})

'''
else:
    stw = F'''
#endif // defined({fguard})

'''
ofs.write(stw)

#############################################################################################################
# List of function return types
ofs.write("// Return types:\n")
for rt in sorted(returnTypes):
    ofs.write(F"// {rt}\n")

# List of functions to make reply messages from different return types
ofs.write("// Reply functions:\n")
for rt in sorted(returnTypes):
    ofs.write(F"// void addReplyResult(OSCMessage& msg, int addrOff, OSCBundle& reply, {rt} v) {{ dbgReplyResult(msg); Serial.println(v); }}\n")

# List of FFT windows
ofs.write('''/*
// === FFT window names ===
typedef struct {
    char* name;
    int16_t* windowData;
    } FFTwindows_t;

''')
for fft in FFTwd:
    ofs.write('const FFTwindows_t FFT%swindows[] = {\n' % fft)
    for wn in FFTwd[fft]:
        ofs.write('\t"%s",AudioWindow%s%s,\n' % (wn,wn,fft))
    ofs.write('}\n\n')
ofs.write('\n\n\n')


# documentation
for cl in sorted(d):
    fd = d[cl]
    s = ''
    nf = 0
    if len(fd) > 1:
        s += '#### %s\n' % (cl,)
        s += '|function|return type|short-form|parameter pattern|parameters|\n'
        s += '|----|----|----|----|----|\n'
        for fi in sorted(fd):
            if '~' not in fi:
                continue
            if 'exclude' not in fd[fi]:
                nf += 1
                pp = fd[fi]['o'].replace('"','').replace('NULL','--')
                ll = fd[fi]['l']
                m = re.match('[^(]*.([^)]*)',ll)
                if m:
                    ll = m.group(1)
                    ll = re.sub('=[^,]*','',ll)
                if 'doc' in fd[fi]:
                    ll = fd[fi]['doc']
                s += '| %s |\n' % ' | '.join((fd[fi]['f'],fd[fi]['r'],fd[fi]['s'],pp,ll))
    if nf > 0:
        ofs.write(s)
        #ofs.write('%d\n' % nf)
ofs.write('*/\n\n\n')

# done - close output file
ofs.write("\n\n")
ofs.close()

# debug stuff
if False:
    for k in opd:
        print(k,len(opd[k]))
        print(opd[k])
    for obj in sorted(objd):
        print(obj)
        for od in objd[obj]:
            print(od)
            if od['shareable'] and 'setting' not in od:
                print('*******************')
            if not od['shareable'] and 'setting' in od:
                print('???????????????????')
    for rsrc in sorted(rsrcd):
        print(rsrc,end=' ')
        if len(rsrcd[rsrc])> 0:
            print(rsrcd[rsrc])
        else:
            print()
    print(['setgAvailable','setgUnshareable']+list(map(lambda s:'setg_'+s.replace(' ','_'),sorted(sets))))
    print(list(map(lambda s:'rsrc_'+s.replace(' ','_'),sorted(rsrcd))))


