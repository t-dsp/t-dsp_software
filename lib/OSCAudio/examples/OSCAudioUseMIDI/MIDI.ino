/* Open Sound Control for Audio Library for Teensy 3.x, 4.x
 * Copyright (c) 2021, Jonathan Oakley, teensy-osc@0akley.co.uk
 *
 * Development of this library was enabled by PJRC.COM, LLC by sales of
 * Teensy and Audio Adaptor boards, implementing libraries, and maintaining
 * the forum at https://forum.pjrc.com/ 
 *
 * Please support PJRC's efforts to develop open source software by 
 * purchasing Teensy or other PJRC products.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice, development funding notice, and this permission
 * notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

// Demo code fragment to illustrate using OSC messages in conjunction with MIDI

#include <OSCBundle.h>
#include "OSCAudioUseMIDI.h"

/*
 * JSON to use in GUI++ for test purposes
 * {"version":1,"settings":{"arduino":{"ProjectName":"SimpleMIDIpoly","Board":{"Platform":"","Board":"","Options":""}},"BiDirDataWebSocketBridge":{},"workspaces":{},"sidebar":{"autoSwitchTabToInfoTab":false},"palette":{},"editor":{},"devTest":{},"IndexedDBfiles":{"testFileNames":"testFile.txt"},"NodeDefGenerator":{},"NodeDefManager":{},"NodeHelpManager":{},"OSC":{"UseDebugLinkName":true}},"workspaces":[{"type":"tab","id":"3629fcd9.ccc604","label":"Main","inputs":0,"outputs":0,"export":true,"isMain":false,"mainNameType":"tabName","mainNameExt":".ino","generateCppDestructor":false,"extraClassDeclarations":"","settings":{"guiEditMode":false,"workspaceBgColor":"#EDFFDF","scaleFactor":0.8,"showGridHminor":false,"showGridHmajor":false,"showGridVminor":false,"showGridVmajor":false,"useCenterBasedPositions":false},"nodes":[{"id":"Main_waveform1","type":"AudioSynthWaveform","name":"wavA1","comment":"","x":60,"y":95,"z":"3629fcd9.ccc604","bgColor":"#E6E0F8","wires":[["Main_mixer4_2:0"]]},{"id":"Main_waveform5","type":"AudioSynthWaveform","name":"wavA2","comment":"","x":60,"y":140,"z":"3629fcd9.ccc604","bgColor":"#E6E0F8","wires":[["Main_mixer4_2:1"]]},{"id":"Main_waveform2","type":"AudioSynthWaveform","name":"wavB1","comment":"","x":60,"y":200,"z":"3629fcd9.ccc604","bgColor":"#E6E0F8","wires":[["Main_mixer4_3:0"]]},{"id":"Main_waveform3","type":"AudioSynthWaveform","name":"wavB2","comment":"","x":60,"y":245,"z":"3629fcd9.ccc604","bgColor":"#E6E0F8","wires":[["Main_mixer4_3:1"]]},{"id":"Main_waveform4","type":"AudioSynthWaveform","name":"wavC1","comment":"","x":65,"y":305,"z":"3629fcd9.ccc604","bgColor":"#E6E0F8","wires":[["Main_mixer4_4:0"]]},{"id":"Main_waveform6","type":"AudioSynthWaveform","name":"wavC2","comment":"","x":65,"y":350,"z":"3629fcd9.ccc604","bgColor":"#E6E0F8","wires":[["Main_mixer4_4:1"]]},{"id":"Main_waveform7","type":"AudioSynthWaveform","name":"wavD1","comment":"","x":65,"y":410,"z":"3629fcd9.ccc604","bgColor":"#E6E0F8","wires":[["Main_mixer4_5:0"]]},{"id":"Main_waveform8","type":"AudioSynthWaveform","name":"wavD2","comment":"","x":65,"y":455,"z":"3629fcd9.ccc604","bgColor":"#E6E0F8","wires":[["Main_mixer4_5:1"]]},{"id":"Main_mixer4_2","type":"AudioMixer4","name":"mixA","comment":"","inputs":"4","x":260,"y":115,"z":"3629fcd9.ccc604","bgColor":"#E6E0F8","wires":[["Main_envelope1:0"]]},{"id":"Main_mixer4_3","type":"AudioMixer4","name":"mixB","comment":"","inputs":"4","x":260,"y":220,"z":"3629fcd9.ccc604","bgColor":"#E6E0F8","wires":[["Main_envelope2:0"]]},{"id":"Main_mixer4_4","type":"AudioMixer4","name":"mixC","comment":"","inputs":"4","x":265,"y":325,"z":"3629fcd9.ccc604","bgColor":"#E6E0F8","wires":[["Main_envelope3:0"]]},{"id":"Main_mixer4_5","type":"AudioMixer4","name":"mixD","comment":"","inputs":"4","x":265,"y":430,"z":"3629fcd9.ccc604","bgColor":"#E6E0F8","wires":[["Main_envelope4:0"]]},{"id":"Main_envelope1","type":"AudioEffectEnvelope","name":"envA","comment":"","x":380,"y":125,"z":"3629fcd9.ccc604","bgColor":"#E6E0F8","wires":[["Main_mixer4_1:0"]]},{"id":"Main_envelope2","type":"AudioEffectEnvelope","name":"envB","comment":"","x":380,"y":230,"z":"3629fcd9.ccc604","bgColor":"#E6E0F8","wires":[["Main_mixer4_1:1"]]},{"id":"Main_envelope3","type":"AudioEffectEnvelope","name":"envC","comment":"","x":385,"y":335,"z":"3629fcd9.ccc604","bgColor":"#E6E0F8","wires":[["Main_mixer4_1:2"]]},{"id":"Main_envelope4","type":"AudioEffectEnvelope","name":"envD","comment":"","x":385,"y":440,"z":"3629fcd9.ccc604","bgColor":"#E6E0F8","wires":[["Main_mixer4_1:3"]]},{"id":"Main_mixer4_1","type":"AudioMixer4","name":"mixer","comment":"","inputs":"4","x":560,"y":230,"z":"3629fcd9.ccc604","bgColor":"#E6E0F8","wires":[["Main_pt8211_2_1:0","Main_pt8211_2_1:1","Main_i2s1:0","Main_i2s1:1"]]},{"id":"Main_pt8211_2_1","type":"AudioOutputPT8211_2","name":"pt8211_2","comment":"","x":720,"y":210,"z":"3629fcd9.ccc604","bgColor":"#E6E0F8","wires":[]},{"id":"Main_i2s1","type":"AudioOutputI2S","name":"i2s","comment":"","x":740,"y":270,"z":"3629fcd9.ccc604","bgColor":"#E6E0F8","wires":[]},{"id":"Main_sgtl5000_1","type":"AudioControlSGTL5000","name":"sgtl5000","comment":"","x":1560,"y":445,"z":"3629fcd9.ccc604","bgColor":"#E6E0F8","wires":[]},{"id":"Main_Button1","type":"UI_Button","name":"use connection to connect wafo to mixer1 input 1","comment":"","w":360,"h":34,"textSize":14,"midiCh":"0","midiId":"0","pressAction":"","repeatPressAction":false,"releaseAction":"","repeatReleaseAction":false,"local":"true","sendCommand":"var addr = \"/teensy1/audio/wafo2mixer1/connect*\"\nvar data = OSC.GetSimpleOSCdata(addr,\"sisi\", \"wafo\", 0, \"mixer1\", 1);\nOSC.SendAsSlipToSerial(data);","x":2200,"y":170,"z":"3629fcd9.ccc604","bgColor":"#F6F8BC","wires":[]},{"id":"Main_Slider1","type":"UI_Slider","name":"ch1","tag":"g 0","comment":"","w":32,"h":281,"textSize":16,"midiCh":"","midiId":"29","orientation":"v","label":"d.val/d.maxVal","minVal":0,"maxVal":100,"val":10,"divVal":1,"fval":10,"sendMode":"m","autoReturn":false,"returnValue":"mid","barFGcolor":"#F87A00","sendFormat":"\"midisend(0xB0,\"+d.midiId+\",\" + d.val + \");\"","sendCommand":"var tags = d.tag.split(' ');\nvar addr = RED.OSC.settings.RootAddress + \"/audio/\"+d.parentGroup.name+\"/\"+tags[0]+\"*\";\nOSC.SendData(OSC.CreateMessageData(addr,\"if\", tags[1], d.val/d.maxVal));","parentGroup":"Main_group1","x":1320,"y":84.28572082519531,"z":"3629fcd9.ccc604","bgColor":"#808080","wires":[]},{"id":"Main_Slider2","type":"UI_Slider","name":"ch2","tag":"g 1","comment":"","w":30,"h":280,"textSize":16,"midiCh":"","midiId":"29","orientation":"v","label":"d.val/d.maxVal","minVal":0,"maxVal":100,"val":10,"divVal":1,"fval":10,"sendMode":"m","autoReturn":false,"returnValue":"mid","barFGcolor":"#F87A00","sendFormat":"\"midisend(0xB0,\"+d.midiId+\",\" + d.val + \");\"","sendCommand":"var tags = d.tag.split(' ');\nvar addr = RED.OSC.settings.RootAddress + \"/audio/\"+d.parentGroup.name+\"/\"+tags[0]+\"*\";\nOSC.SendData(OSC.CreateMessageData(addr,\"if\", tags[1], d.val/d.maxVal));","parentGroup":"Main_group1","x":1370,"y":85,"z":"3629fcd9.ccc604","bgColor":"#808080","wires":[]},{"id":"Main_Slider3","type":"UI_Slider","name":"ch3","tag":"g 2","comment":"","w":30,"h":280,"textSize":16,"midiCh":"","midiId":"29","orientation":"v","label":"d.val/d.maxVal","minVal":0,"maxVal":100,"val":0,"divVal":1,"fval":0,"sendMode":"m","autoReturn":false,"returnValue":"mid","barFGcolor":"#F87A00","sendFormat":"\"midisend(0xB0,\"+d.midiId+\",\" + d.val + \");\"","sendCommand":"var tags = d.tag.split(' ');\nvar addr = RED.OSC.settings.RootAddress + \"/audio/\"+d.parentGroup.name+\"/\"+tags[0]+\"*\";\nOSC.SendData(OSC.CreateMessageData(addr,\"if\", tags[1], d.val/d.maxVal));","parentGroup":"Main_group1","x":1420,"y":84.28572082519531,"z":"3629fcd9.ccc604","bgColor":"#808080","wires":[]},{"id":"Main_Slider4","type":"UI_Slider","name":"ch4","tag":"g 3","comment":"","w":30,"h":280,"textSize":16,"midiCh":"","midiId":"29","orientation":"v","label":"d.val/d.maxVal","minVal":0,"maxVal":100,"val":10,"divVal":1,"fval":10,"sendMode":"m","autoReturn":false,"returnValue":"mid","barFGcolor":"#F87A00","sendFormat":"\"midisend(0xB0,\"+d.midiId+\",\" + d.val + \");\"","sendCommand":"var tags = d.tag.split(' ');\nvar addr = RED.OSC.settings.RootAddress + \"/audio/\"+d.parentGroup.name+\"/\"+tags[0]+\"*\";\nOSC.SendData(OSC.CreateMessageData(addr,\"if\", tags[1], d.val/d.maxVal));","parentGroup":"Main_group1","x":1470,"y":84.28572082519531,"z":"3629fcd9.ccc604","bgColor":"#808080","wires":[]},{"id":"Main_Button2","type":"UI_Button","name":"create new AudioSynthWaveForm wafo","comment":"","w":275,"h":34,"textSize":14,"midiCh":"0","midiId":"0","pressAction":"","repeatPressAction":false,"releaseAction":"","repeatReleaseAction":false,"local":"true","sendCommand":"var addr = \"/teensy1/dynamic/createObject*\"\nvar data = OSC.GetSimpleOSCdata(addr,\"ss\", \"AudioSynthWaveform\", \"wafo\");\nOSC.SendAsSlipToSerial(data);","x":2260,"y":90,"z":"3629fcd9.ccc604","bgColor":"#F6F8BC","wires":[]},{"id":"Main_Button3","type":"UI_Button","name":"create connection wafo2mixer1","comment":"","w":223,"h":30,"textSize":14,"midiCh":"0","midiId":"0","pressAction":"","repeatPressAction":false,"releaseAction":"","repeatReleaseAction":false,"local":"true","sendCommand":"var addr = \"/teensy1/dynamic/createConn*\"\nvar data = OSC.GetSimpleOSCdata(addr,\"s\", \"wafo2mixer1\");\nOSC.SendAsSlipToSerial(data);","x":2260,"y":130,"z":"3629fcd9.ccc604","bgColor":"#F6F8BC","wires":[]},{"id":"Main_Button4","type":"UI_Button","name":"do all above in one go","comment":"","w":157,"h":34,"textSize":14,"midiCh":"0","midiId":"0","pressAction":"","repeatPressAction":false,"releaseAction":"","repeatReleaseAction":false,"local":"true","sendCommand":"var data = osc.writeBundle({\r\n        timeTag: osc.timeTag(0),\r\n        packets: [\r\n            {\r\n                address: \"/teensy1/dynamic/createObject*\",\r\n                args: [\r\n\t\t\t\t\t{type: \"s\", value: \"AudioSynthWaveform\"},\r\n\t\t\t\t\t{type: \"s\", value: \"wafo\"}\r\n                ]\r\n            },\r\n            {\r\n                address: \"/teensy1/dynamic/createConn*\",\r\n                args: [\r\n                    {type: \"s\", value: \"wafo2mixer1\"}\r\n                ]\r\n            },\r\n            {\r\n                address: \"/teensy1/audio/wafo2mixer1/connect*\",\r\n                args: [\r\n                    {type: \"s\", value: \"wafo\"},\r\n\t\t\t\t\t{type: \"i\", value: 0},\r\n\t\t\t\t\t{type: \"s\", value: \"mixer1\"},\r\n\t\t\t\t\t{type: \"i\", value: 1}\r\n                ]\r\n            }\r\n\t\t\t\r\n        ]\r\n    });\r\nOSC.SendAsSlipToSerial(data);","x":2280,"y":210,"z":"3629fcd9.ccc604","bgColor":"#F6F8BC","wires":[]},{"id":"Main_group1","type":"group","name":"mixer","comment":"","w":228,"h":472,"textSize":14,"nodes":["Main_Slider1","Main_Slider2","Main_Slider3","Main_Slider4","Main_Button6","Main_Button13"],"border_color":"rgba(153,153,153,1)","individualListBoxMode":true,"exportAsClass":true,"x":1300,"y":30,"z":"3629fcd9.ccc604","bgColor":"rgba(221,255,187,1)","wires":[]},{"id":"Main_Button6","type":"UI_Button","name":"set all mixer gains to 0.10","tag":"g","comment":"","w":176,"h":34,"textSize":14,"midiCh":"0","midiId":"0","pressAction":"","repeatPressAction":false,"releaseAction":"","repeatReleaseAction":false,"local":"true","sendCommand":"var parentName = d.parentGroup.name;\r\nvar gain = 0.10;\r\nvar addr = RED.OSC.settings.RootAddress + \"/audio/\"+d.parentGroup.name+\"/\"+d.tag+\"*\";\r\n\r\nvar bundle = OSC.CreateBundle(0);\r\nbundle.packets.push(OSC.CreatePacket(addr,\"if\",0,gain));\r\nbundle.packets.push(OSC.CreatePacket(addr,\"if\",1,gain));\r\nbundle.packets.push(OSC.CreatePacket(addr,\"if\",2,gain));\r\nbundle.packets.push(OSC.CreatePacket(addr,\"if\",3,gain));\r\nOSC.SendData(OSC.CreateBundleData(bundle));\r\n\r\n// update sliders in ui\r\nfor (var i=0; i<d.parentGroup.nodes.length; i++) {\r\n    d.parentGroup.nodes[i].val = gain*d.parentGroup.nodes[i].maxVal;\r\n    d.parentGroup.nodes[i].dirty=true;\r\n}\r\nRED.view.redraw();","parentGroup":"Main_group1","x":1320,"y":399.2857208251953,"z":"3629fcd9.ccc604","bgColor":"#F6F8BC","wires":[]},{"id":"Main_Button7","type":"UI_Button","name":"Button","comment":"","w":100,"h":34,"textSize":14,"midiCh":"0","midiId":"0","pressAction":"","repeatPressAction":false,"releaseAction":"","repeatReleaseAction":false,"local":"true","sendCommand":"var addr = \"/teensy1/dynamic/createObject*\"\nvar data = OSC.GetSimpleOSCdata(addr,\"ss\", \"AudioSynthWaveform\", \"   \");\nOSC.SendAsSlipToSerial(data);","x":2340,"y":280,"z":"3629fcd9.ccc604","bgColor":"#F6F8BC","wires":[]},{"id":"Main_Button8","type":"UI_Button","name":"rename waveform2 to wafo1","comment":"","w":275,"h":34,"textSize":14,"midiCh":"0","midiId":"0","pressAction":"","repeatPressAction":false,"releaseAction":"","repeatReleaseAction":false,"local":"true","sendCommand":"var addr = \"/teensy1/dynamic/ren*\"\nvar data = OSC.GetSimpleOSCdata(addr,\"ss\", \"waveform2\", \"wafo\");\nOSC.SendAsSlipToSerial(data);","x":2295,"y":370,"z":"3629fcd9.ccc604","bgColor":"#F6F8BC","wires":[]},{"id":"Main_Piano3","type":"UI_Piano","name":"Piano octave 4","tag":"wavA","comment":"","w":210,"h":130,"textSize":14,"midiCh":"0","midiId":"0","octave":4,"sendCommand":"// this table goes to octave -1\nvar noteFreqs = [8.176, 8.662, 9.177, 9.723, 10.301, 10.913, 11.562, 12.25, 12.978, 13.75, 14.568, 15.434, 16.352, 17.324, 18.354, 19.445, 20.602, 21.827, 23.125, 24.5, 25.957, 27.5, 29.135, 30.868, 32.703, 34.648, 36.708, 38.891, 41.203, 43.654, 46.249, 48.999, 51.913, 55, 58.27, 61.735, 65.406, 69.296, 73.416, 77.782, 82.407, 87.307, 92.499, 97.999, 103.826, 110, 116.541, 123.471, 130.813, 138.591, 146.832, 155.563, 164.814, 174.614, 184.997, 195.998, 207.652, 220, 233.082, 246.942, 261.626, 277.183, 293.665, 311.127, 329.628, 349.228, 369.994, 391.995, 415.305, 440, 466.164, 493.883, 523.251, 554.365, 587.33, 622.254, 659.255, 698.456, 739.989, 783.991, 830.609, 880, 932.328, 987.767, 1046.502, 1108.731, 1174.659, 1244.508, 1318.51, 1396.913, 1479.978, 1567.982, 1661.219, 1760, 1864.655, 1975.533, 2093.005, 2217.461, 2349.318, 2489.016, 2637.02, 2793.826, 2959.955, 3135.963, 3322.438, 3520, 3729.31, 3951.066, 4186.009, 4434.922, 4698.636, 4978.032, 5274.041, 5587.652, 5919.911, 6271.927, 6644.875, 7040, 7458.62, 7902.133, 8372.018, 8869.844, 9397.273, 9956.063, 10548.08, 11175.3, 11839.82, 12543.85];\nvar freq = noteFreqs[(d.octave+1)*12 + d.keyIndex];\n\n// send in bundle with two commands frequency and noteOn/Off\nvar bundle = OSC.CreateBundle(0);\nbundle.packets.push(OSC.CreatePacket(RED.OSC.settings.RootAddress + '/audio/'+d.tag+'1/be',\"ffi\",0.5, freq,2));\nbundle.packets.push(OSC.CreatePacket(RED.OSC.settings.RootAddress + '/audio/'+d.tag+'2/be',\"ffi\",0.5, freq/2,2));\nif (d.pressed === true)\n    bundle.packets.push(OSC.CreatePacket(RED.OSC.settings.RootAddress + '/audio/'+d.parentGroup.name+'/noteOn',\"\"));\nelse\n    bundle.packets.push(OSC.CreatePacket(RED.OSC.settings.RootAddress + '/audio/'+d.parentGroup.name+'/noteOf*',\"\"));\n\nOSC.SendData(OSC.CreateBundleData(bundle));\n\n","headerHeight":30,"whiteKeysColor":"#FFFFFF","blackKeysColor":"#A0A0A0","blackKeysWidthDiff":6,"x":280,"y":630,"blackKeyLabelsVisible":true,"whiteKeyLabelsVisible":true,"parentGroup":"Main_group2","z":"3629fcd9.ccc604","bgColor":"#F6F8BC","wires":[]},{"id":"Main_Button9","type":"UI_Button","name":"enable","tag":"enabl","comment":"","w":100,"h":34,"textSize":14,"midiCh":"0","midiId":"0","pressAction":"","repeatPressAction":false,"releaseAction":"","repeatReleaseAction":false,"local":"true","sendCommand":"var addr = RED.OSC.settings.RootAddress + \"/audio/\"+d.parentGroup.name+\"/\"+d.tag+\"*\";\nOSC.SendData(OSC.CreateMessageData(addr,''));","parentGroup":"Main_group7","x":1555,"y":60,"z":"3629fcd9.ccc604","bgColor":"#F6F8BC","wires":[]},{"id":"Main_Slider15","type":"UI_Slider","name":"volume","tag":"vol","comment":"","w":30,"h":265,"textSize":14,"midiCh":"0","midiId":"0","orientation":"v","label":"d.val","minVal":0,"maxVal":100,"val":18,"divVal":1,"fval":18,"sendMode":"r","autoReturn":false,"returnValue":"mid","barFGcolor":"#F6F8BC","sendFormat":"","sendCommand":"var addr = RED.OSC.settings.RootAddress + \"/audio/\"+d.parentGroup.name+\"/\"+d.tag+\"*\";\nOSC.SendData(OSC.CreateMessageData(addr,'f',d.val/d.maxVal));","parentGroup":"Main_group7","x":1590,"y":130,"z":"3629fcd9.ccc604","bgColor":"#808080","wires":[]},{"id":"Main_Button13","type":"UI_Button","name":"set all mixer gains to 0","tag":"","comment":"","w":176,"h":34,"textSize":14,"midiCh":"0","midiId":"0","pressAction":"","repeatPressAction":false,"releaseAction":"","repeatReleaseAction":false,"local":"true","sendCommand":"var parentName = d.parentGroup.name;\r\nvar gain = 0;\r\nvar addr = RED.OSC.settings.RootAddress + \"/audio/\"+d.parentGroup.name+\"/\"+d.tag+\"*\";\r\n\r\nvar bundle = OSC.CreateBundle(0);\r\nbundle.packets.push(OSC.CreatePacket(addr,\"if\",0,gain));\r\nbundle.packets.push(OSC.CreatePacket(addr,\"if\",1,gain));\r\nbundle.packets.push(OSC.CreatePacket(addr,\"if\",2,gain));\r\nbundle.packets.push(OSC.CreatePacket(addr,\"if\",3,gain));\r\nOSC.SendData(OSC.CreateBundleData(bundle));\r\n\r\n// update sliders in ui\r\nfor (var i=0; i<d.parentGroup.nodes.length; i++) {\r\n    d.parentGroup.nodes[i].val = gain*d.parentGroup.nodes[i].maxVal;\r\n    d.parentGroup.nodes[i].dirty=true;\r\n}\r\nRED.view.redraw();","parentGroup":"Main_group1","x":1320,"y":440,"z":"3629fcd9.ccc604","bgColor":"#F6F8BC","wires":[]},{"id":"Main_Slider28","type":"UI_Slider","name":"Delay","tag":"del*","comment":"","w":30,"h":211,"textSize":14,"midiCh":"0","midiId":100,"orientation":"v","label":"d.val*10 + \"ms\"","minVal":0,"maxVal":100,"val":0,"divVal":1,"fval":0,"sendMode":"r","autoReturn":false,"returnValue":"mid","barFGcolor":"#FF0000","sendFormat":"\"midisend(0xB0,\"+d.midiId+\",\" + d.val + \");\"","sendCommand":"var addr = RED.OSC.settings.RootAddress + \"/audio/\"+d.parentGroup.name+\"/\"+d.tag;\nOSC.SendData(OSC.CreateMessageData(addr,\"f\", (d.val*10)));","parentGroup":"Main_group8","x":935,"y":100,"z":"3629fcd9.ccc604","bgColor":"#808080","wires":[]},{"id":"Main_Slider29","type":"UI_Slider","name":"Attack","tag":"a*","comment":"","w":30,"h":212,"textSize":14,"midiCh":"0","midiId":101,"orientation":"v","label":"d.val*10 + \"ms\"","minVal":0,"maxVal":100,"val":18,"divVal":1,"fval":18,"sendMode":"r","autoReturn":false,"returnValue":"mid","barFGcolor":"#FFFF00","sendFormat":"\"midisend(0xB0,\"+d.midiId+\",\" + d.val + \");\"","sendCommand":"var addr = RED.OSC.settings.RootAddress + \"/audio/\"+d.parentGroup.name+\"/\"+d.tag;\nOSC.SendData(OSC.CreateMessageData(addr,\"f\", (d.val*10)));","parentGroup":"Main_group8","x":990,"y":100,"z":"3629fcd9.ccc604","bgColor":"#808080","wires":[]},{"id":"Main_Slider30","type":"UI_Slider","name":"Hold","tag":"h*","comment":"","w":30,"h":215,"textSize":14,"midiCh":"0","midiId":102,"orientation":"v","label":"d.val*10 + \"ms\"","minVal":0,"maxVal":100,"val":6,"divVal":1,"fval":6,"sendMode":"r","autoReturn":false,"returnValue":"mid","barFGcolor":"#00FF00","sendFormat":"\"midisend(0xB0,\"+d.midiId+\",\" + d.val + \");\"","sendCommand":"var addr = RED.OSC.settings.RootAddress + \"/audio/\"+d.parentGroup.name+\"/\"+d.tag;\nOSC.SendData(OSC.CreateMessageData(addr,\"f\", (d.val*10)));","parentGroup":"Main_group8","x":1045,"y":100,"z":"3629fcd9.ccc604","bgColor":"#808080","wires":[]},{"id":"Main_Slider31","type":"UI_Slider","name":"Decay","tag":"dec*","comment":"","w":30,"h":215,"textSize":14,"midiCh":"0","midiId":103,"orientation":"v","label":"d.val*10 + \"ms\"","minVal":0,"maxVal":100,"val":36,"divVal":1,"fval":36,"sendMode":"r","autoReturn":false,"returnValue":"mid","barFGcolor":"#00FFFF","sendFormat":"\"midisend(0xB0,\"+d.midiId+\",\" + d.val + \");\"","sendCommand":"var addr = RED.OSC.settings.RootAddress + \"/audio/\"+d.parentGroup.name+\"/\"+d.tag;\nOSC.SendData(OSC.CreateMessageData(addr,\"f\", (d.val*10)));","parentGroup":"Main_group8","x":1100,"y":100,"z":"3629fcd9.ccc604","bgColor":"#808080","wires":[]},{"id":"Main_Slider32","type":"UI_Slider","name":"Sustain","tag":"s*","comment":"","w":30,"h":214,"textSize":14,"midiCh":"0","midiId":104,"orientation":"v","label":"d.val+\"%\"","minVal":0,"maxVal":100,"val":37,"divVal":1,"fval":37,"sendMode":"r","autoReturn":false,"returnValue":"mid","barFGcolor":"#0000FF","sendFormat":"\"midisend(0xB0,\"+d.midiId+\",\" + d.val + \");\"","sendCommand":"var addr = RED.OSC.settings.RootAddress + \"/audio/\"+d.parentGroup.name+\"/\"+d.tag;\nOSC.SendData(OSC.CreateMessageData(addr,\"f\", d.val/100.0));","parentGroup":"Main_group8","x":1155,"y":100,"z":"3629fcd9.ccc604","bgColor":"#808080","wires":[]},{"id":"Main_Slider33","type":"UI_Slider","name":"Release","tag":"release","comment":"","w":30,"h":214,"textSize":14,"midiCh":"0","midiId":105,"orientation":"v","label":"d.val*10 + \"ms\"","minVal":0,"maxVal":100,"val":37,"divVal":1,"fval":37,"sendMode":"r","autoReturn":false,"returnValue":"mid","barFGcolor":"#FF00FF","sendFormat":"\"midisend(0xB0,\"+d.midiId+\",\" + d.val + \");\"","sendCommand":"var addr = RED.OSC.settings.RootAddress + \"/audio/\"+d.parentGroup.name+\"/\"+d.tag;\nOSC.SendData(OSC.CreateMessageData(addr,\"f\", (d.val*10)));","parentGroup":"Main_group8","x":1210,"y":100,"z":"3629fcd9.ccc604","bgColor":"#808080","wires":[]},{"id":"Main_group8","type":"group","name":"env*","tag":"","comment":"","w":354,"h":305,"textSize":14,"nodes":["Main_Slider28","Main_Slider29","Main_Slider30","Main_Slider31","Main_Slider32","Main_Slider33"],"border_color":"rgba(153,153,153,1)","individualListBoxMode":true,"exportAsClass":true,"x":915,"y":40,"z":"3629fcd9.ccc604","bgColor":"rgba(221,255,187,1)","wires":[]},{"id":"Main_group2","type":"group","name":"envA","tag":"waveform1","comment":"","w":669,"h":182,"textSize":14,"nodes":["Main_Piano3","Main_Piano1","Main_Piano2"],"border_color":"rgba(153,153,153,1)","individualListBoxMode":true,"exportAsClass":true,"x":50,"y":590,"z":"3629fcd9.ccc604","bgColor":"rgba(221,255,187,1)","wires":[]},{"id":"Main_group7","type":"group","name":"sgtl5000","comment":"","w":119,"h":403,"textSize":14,"nodes":["Main_Button9","Main_Slider15"],"border_color":"rgba(153,153,153,1)","individualListBoxMode":true,"exportAsClass":true,"x":1550,"y":25,"z":"3629fcd9.ccc604","bgColor":"rgba(221,255,187,1)","wires":[]},{"id":"Main_group3","type":"group","name":"mix*","tag":"","comment":"","w":228,"h":472,"textSize":14,"nodes":["Main_Slider5","Main_Slider6","Main_Slider7","Main_Slider8","Main_Button5","Main_Button10"],"border_color":"rgba(153,153,153,1)","individualListBoxMode":true,"exportAsClass":true,"x":925,"y":365,"z":"3629fcd9.ccc604","bgColor":"rgba(221,255,187,1)","wires":[]},{"id":"Main_Slider5","type":"UI_Slider","name":"ch1","tag":"g 0","comment":"","w":32,"h":281,"textSize":16,"midiCh":"","midiId":"29","orientation":"v","label":"d.val/d.maxVal","minVal":0,"maxVal":100,"val":77,"divVal":1,"fval":77,"sendMode":"m","autoReturn":false,"returnValue":"mid","barFGcolor":"#F87A00","sendFormat":"\"midisend(0xB0,\"+d.midiId+\",\" + d.val + \");\"","sendCommand":"var tags = d.tag.split(' ');\nvar addr = RED.OSC.settings.RootAddress + \"/audio/\"+d.parentGroup.name+\"/\"+tags[0]+\"*\";\nOSC.SendData(OSC.CreateMessageData(addr,\"if\", tags[1], d.val/d.maxVal));","parentGroup":"Main_group3","x":945,"y":419.2857208251953,"z":"3629fcd9.ccc604","bgColor":"#808080","wires":[]},{"id":"Main_Slider6","type":"UI_Slider","name":"ch2","tag":"g 1","comment":"","w":30,"h":280,"textSize":16,"midiCh":"","midiId":"29","orientation":"v","label":"d.val/d.maxVal","minVal":0,"maxVal":100,"val":32,"divVal":1,"fval":32,"sendMode":"m","autoReturn":false,"returnValue":"mid","barFGcolor":"#F87A00","sendFormat":"\"midisend(0xB0,\"+d.midiId+\",\" + d.val + \");\"","sendCommand":"var tags = d.tag.split(' ');\nvar addr = RED.OSC.settings.RootAddress + \"/audio/\"+d.parentGroup.name+\"/\"+tags[0]+\"*\";\nOSC.SendData(OSC.CreateMessageData(addr,\"if\", tags[1], d.val/d.maxVal));","parentGroup":"Main_group3","x":995,"y":420,"z":"3629fcd9.ccc604","bgColor":"#808080","wires":[]},{"id":"Main_Slider7","type":"UI_Slider","name":"ch3","tag":"g 2","comment":"","w":30,"h":280,"textSize":16,"midiCh":"","midiId":"29","orientation":"v","label":"d.val/d.maxVal","minVal":0,"maxVal":100,"val":0,"divVal":1,"fval":0,"sendMode":"m","autoReturn":false,"returnValue":"mid","barFGcolor":"#F87A00","sendFormat":"\"midisend(0xB0,\"+d.midiId+\",\" + d.val + \");\"","sendCommand":"var tags = d.tag.split(' ');\nvar addr = RED.OSC.settings.RootAddress + \"/audio/\"+d.parentGroup.name+\"/\"+tags[0]+\"*\";\nOSC.SendData(OSC.CreateMessageData(addr,\"if\", tags[1], d.val/d.maxVal));","parentGroup":"Main_group3","x":1045,"y":419.2857208251953,"z":"3629fcd9.ccc604","bgColor":"#808080","wires":[]},{"id":"Main_Slider8","type":"UI_Slider","name":"ch4","tag":"g 3","comment":"","w":30,"h":280,"textSize":16,"midiCh":"","midiId":"29","orientation":"v","label":"d.val/d.maxVal","minVal":0,"maxVal":100,"val":10,"divVal":1,"fval":10,"sendMode":"m","autoReturn":false,"returnValue":"mid","barFGcolor":"#F87A00","sendFormat":"\"midisend(0xB0,\"+d.midiId+\",\" + d.val + \");\"","sendCommand":"var tags = d.tag.split(' ');\nvar addr = RED.OSC.settings.RootAddress + \"/audio/\"+d.parentGroup.name+\"/\"+tags[0]+\"*\";\nOSC.SendData(OSC.CreateMessageData(addr,\"if\", tags[1], d.val/d.maxVal));","parentGroup":"Main_group3","x":1095,"y":419.2857208251953,"z":"3629fcd9.ccc604","bgColor":"#808080","wires":[]},{"id":"Main_Button5","type":"UI_Button","name":"set all mixer gains to 0.10","tag":"g","comment":"","w":176,"h":34,"textSize":14,"midiCh":"0","midiId":"0","pressAction":"","repeatPressAction":false,"releaseAction":"","repeatReleaseAction":false,"local":"true","sendCommand":"var parentName = d.parentGroup.name;\r\nvar gain = 0.10;\r\nvar addr = RED.OSC.settings.RootAddress + \"/audio/\"+d.parentGroup.name+\"/\"+d.tag+\"*\";\r\n\r\nvar bundle = OSC.CreateBundle(0);\r\nbundle.packets.push(OSC.CreatePacket(addr,\"if\",0,gain));\r\nbundle.packets.push(OSC.CreatePacket(addr,\"if\",1,gain));\r\nbundle.packets.push(OSC.CreatePacket(addr,\"if\",2,gain));\r\nbundle.packets.push(OSC.CreatePacket(addr,\"if\",3,gain));\r\nOSC.SendData(OSC.CreateBundleData(bundle));\r\n\r\n// update sliders in ui\r\nfor (var i=0; i<d.parentGroup.nodes.length; i++) {\r\n    d.parentGroup.nodes[i].val = gain*d.parentGroup.nodes[i].maxVal;\r\n    d.parentGroup.nodes[i].dirty=true;\r\n}\r\nRED.view.redraw();","parentGroup":"Main_group3","x":945,"y":734.2857208251953,"z":"3629fcd9.ccc604","bgColor":"#F6F8BC","wires":[]},{"id":"Main_Button10","type":"UI_Button","name":"set all mixer gains to 0","tag":"","comment":"","w":176,"h":34,"textSize":14,"midiCh":"0","midiId":"0","pressAction":"","repeatPressAction":false,"releaseAction":"","repeatReleaseAction":false,"local":"true","sendCommand":"var parentName = d.parentGroup.name;\r\nvar gain = 0;\r\nvar addr = RED.OSC.settings.RootAddress + \"/audio/\"+d.parentGroup.name+\"/\"+d.tag+\"*\";\r\n\r\nvar bundle = OSC.CreateBundle(0);\r\nbundle.packets.push(OSC.CreatePacket(addr,\"if\",0,gain));\r\nbundle.packets.push(OSC.CreatePacket(addr,\"if\",1,gain));\r\nbundle.packets.push(OSC.CreatePacket(addr,\"if\",2,gain));\r\nbundle.packets.push(OSC.CreatePacket(addr,\"if\",3,gain));\r\nOSC.SendData(OSC.CreateBundleData(bundle));\r\n\r\n// update sliders in ui\r\nfor (var i=0; i<d.parentGroup.nodes.length; i++) {\r\n    d.parentGroup.nodes[i].val = gain*d.parentGroup.nodes[i].maxVal;\r\n    d.parentGroup.nodes[i].dirty=true;\r\n}\r\nRED.view.redraw();","parentGroup":"Main_group3","x":945,"y":775,"z":"3629fcd9.ccc604","bgColor":"#F6F8BC","wires":[]},{"id":"Main_Piano1","type":"UI_Piano","name":"Piano octave 3","tag":"wavA","comment":"","w":210,"h":130,"textSize":14,"midiCh":"0","midiId":"0","octave":3,"sendCommand":"// this table goes to octave -1\nvar noteFreqs = [8.176, 8.662, 9.177, 9.723, 10.301, 10.913, 11.562, 12.25, 12.978, 13.75, 14.568, 15.434, 16.352, 17.324, 18.354, 19.445, 20.602, 21.827, 23.125, 24.5, 25.957, 27.5, 29.135, 30.868, 32.703, 34.648, 36.708, 38.891, 41.203, 43.654, 46.249, 48.999, 51.913, 55, 58.27, 61.735, 65.406, 69.296, 73.416, 77.782, 82.407, 87.307, 92.499, 97.999, 103.826, 110, 116.541, 123.471, 130.813, 138.591, 146.832, 155.563, 164.814, 174.614, 184.997, 195.998, 207.652, 220, 233.082, 246.942, 261.626, 277.183, 293.665, 311.127, 329.628, 349.228, 369.994, 391.995, 415.305, 440, 466.164, 493.883, 523.251, 554.365, 587.33, 622.254, 659.255, 698.456, 739.989, 783.991, 830.609, 880, 932.328, 987.767, 1046.502, 1108.731, 1174.659, 1244.508, 1318.51, 1396.913, 1479.978, 1567.982, 1661.219, 1760, 1864.655, 1975.533, 2093.005, 2217.461, 2349.318, 2489.016, 2637.02, 2793.826, 2959.955, 3135.963, 3322.438, 3520, 3729.31, 3951.066, 4186.009, 4434.922, 4698.636, 4978.032, 5274.041, 5587.652, 5919.911, 6271.927, 6644.875, 7040, 7458.62, 7902.133, 8372.018, 8869.844, 9397.273, 9956.063, 10548.08, 11175.3, 11839.82, 12543.85];\nvar freq = noteFreqs[(d.octave+1)*12 + d.keyIndex];\n\n// send in bundle with two commands frequency and noteOn/Off\nvar bundle = OSC.CreateBundle(0);\nbundle.packets.push(OSC.CreatePacket(RED.OSC.settings.RootAddress + '/audio/'+d.tag+'1/be',\"ffi\",0.5, freq,2));\nbundle.packets.push(OSC.CreatePacket(RED.OSC.settings.RootAddress + '/audio/'+d.tag+'2/be',\"ffi\",0.5, freq/2,2));\nif (d.pressed === true)\n    bundle.packets.push(OSC.CreatePacket(RED.OSC.settings.RootAddress + '/audio/'+d.parentGroup.name+'/noteOn',\"\"));\nelse\n    bundle.packets.push(OSC.CreatePacket(RED.OSC.settings.RootAddress + '/audio/'+d.parentGroup.name+'/noteOf*',\"\"));\n\nOSC.SendData(OSC.CreateBundleData(bundle));\n\n","headerHeight":30,"whiteKeysColor":"#FFFFFF","blackKeysColor":"#A0A0A0","blackKeysWidthDiff":6,"x":70,"y":630,"blackKeyLabelsVisible":true,"whiteKeyLabelsVisible":true,"parentGroup":"Main_group2","z":"3629fcd9.ccc604","bgColor":"#F6F8BC","wires":[]},{"id":"Main_Piano2","type":"UI_Piano","name":"Piano octave 5","tag":"wavA","comment":"","w":210,"h":130,"textSize":14,"midiCh":"0","midiId":"0","octave":5,"sendCommand":"// this table goes to octave -1\nvar noteFreqs = [8.176, 8.662, 9.177, 9.723, 10.301, 10.913, 11.562, 12.25, 12.978, 13.75, 14.568, 15.434, 16.352, 17.324, 18.354, 19.445, 20.602, 21.827, 23.125, 24.5, 25.957, 27.5, 29.135, 30.868, 32.703, 34.648, 36.708, 38.891, 41.203, 43.654, 46.249, 48.999, 51.913, 55, 58.27, 61.735, 65.406, 69.296, 73.416, 77.782, 82.407, 87.307, 92.499, 97.999, 103.826, 110, 116.541, 123.471, 130.813, 138.591, 146.832, 155.563, 164.814, 174.614, 184.997, 195.998, 207.652, 220, 233.082, 246.942, 261.626, 277.183, 293.665, 311.127, 329.628, 349.228, 369.994, 391.995, 415.305, 440, 466.164, 493.883, 523.251, 554.365, 587.33, 622.254, 659.255, 698.456, 739.989, 783.991, 830.609, 880, 932.328, 987.767, 1046.502, 1108.731, 1174.659, 1244.508, 1318.51, 1396.913, 1479.978, 1567.982, 1661.219, 1760, 1864.655, 1975.533, 2093.005, 2217.461, 2349.318, 2489.016, 2637.02, 2793.826, 2959.955, 3135.963, 3322.438, 3520, 3729.31, 3951.066, 4186.009, 4434.922, 4698.636, 4978.032, 5274.041, 5587.652, 5919.911, 6271.927, 6644.875, 7040, 7458.62, 7902.133, 8372.018, 8869.844, 9397.273, 9956.063, 10548.08, 11175.3, 11839.82, 12543.85];\nvar freq = noteFreqs[(d.octave+1)*12 + d.keyIndex];\n\n// send in bundle with two commands frequency and noteOn/Off\nvar bundle = OSC.CreateBundle(0);\nbundle.packets.push(OSC.CreatePacket(RED.OSC.settings.RootAddress + '/audio/'+d.tag+'1/be',\"ffi\",0.5, freq,2));\nbundle.packets.push(OSC.CreatePacket(RED.OSC.settings.RootAddress + '/audio/'+d.tag+'2/be',\"ffi\",0.5, freq/2,2));\nif (d.pressed === true)\n    bundle.packets.push(OSC.CreatePacket(RED.OSC.settings.RootAddress + '/audio/'+d.parentGroup.name+'/noteOn',\"\"));\nelse\n    bundle.packets.push(OSC.CreatePacket(RED.OSC.settings.RootAddress + '/audio/'+d.parentGroup.name+'/noteOf*',\"\"));\n\nOSC.SendData(OSC.CreateBundleData(bundle));\n\n","headerHeight":30,"whiteKeysColor":"#FFFFFF","blackKeysColor":"#A0A0A0","blackKeysWidthDiff":6,"x":490,"y":630,"blackKeyLabelsVisible":true,"whiteKeyLabelsVisible":true,"parentGroup":"Main_group2","z":"3629fcd9.ccc604","bgColor":"#F6F8BC","wires":[]}]}],"nodeAddons":{}}
 */

// one octave of well-tempered frequencies, starting at MIDI note 60 (middle C)
float wellTempered[] = {261.62557, 277.18263, 293.66477, 311.12698, 329.62756, 349.22823, 369.99442, 391.99544, 415.3047, 440, 466.16376, 493.8833};

float note2freq(byte note,float* scale)
{
  float freq;
  byte idx = note % 12;
  int octave;
  
  freq = scale[idx];
  octave = (note - idx - 60)/12; // figure out octave
  if (octave > 0)
    freq *= (float)(1<<octave);
  else if (octave < 0)
    freq /= (float)(1<<(-octave));

  return freq;
}

void dbgMIDI(byte ch, byte typ, byte d1, byte d2)
{
    Serial.printf("%02X: %02X,%02X,%02X\n",
                  ch,typ,d1,d2);
}

void dbgMIDIget(void)
{
    dbgMIDI(usbMIDI.getChannel(),
            usbMIDI.getType(),
            usbMIDI.getData1(),
            usbMIDI.getData2());
}


#define AVAIL 0xFF
byte allocate[4];
// return index in allocate array of available voice,
// or one that's already playing this note. Also
// mark slot as "taken" by this note.
int allocateMIDI(byte note)
{
  size_t i;
  for (i=COUNT_OF(allocate)-1;i>=0;i--)
    if (AVAIL == allocate[i] || note == allocate[i])
    {
      allocate[i] = note;
      break;
    }

  return i;
}

// return index of allocation array where note
// was allocated, and mark as available
int deallocateMIDI(byte note)
{
  size_t i;
  for (i=COUNT_OF(allocate)-1;i>=0;i--)
    if (note == allocate[i])
    {
      allocate[i] = AVAIL;
      break;
    }

  return i;
}


void OSCnoteOn(byte channel, byte note, byte velocity)
{
  float freq;
  OSCBundle bndl;
  OSCBundle reply;
  int slot = allocateMIDI(note);

  if (slot >= 0)
  {
    char buf[50];

    Serial.print("\nNote on: ");
    freq = note2freq(note,wellTempered);
    Serial.println(freq);
    dbgMIDI(channel,0x80,note,velocity);  
  
    uint32_t st = micros();
    sprintf(buf,"/teensy1/audio/wav%c1/be",slot+'A');
    bndl.add(buf).add(velocity/127.0f).add(freq).add(2);
    sprintf(buf,"/teensy1/audio/wav%c2/be",slot+'A');
    bndl.add(buf).add(velocity/127.0f).add(freq/2).add(2);
    sprintf(buf,"/teensy1/audio/env%c/noteOn",slot+'A');
    bndl.add(buf);
    //bndl.send(Serial); // for debug!
    reply.add("/noteOn");
    processBundle(&bndl,reply);
    st = micros() - st;
    Serial.printf("OSC processing took %d us\n",st);
  }
}


void OSCnoteOff(byte channel, byte note, byte velocity)
{
  OSCBundle bndl;
  OSCBundle reply;
  int slot = deallocateMIDI(note);

  Serial.print("\nNote off: ");
  dbgMIDI(channel,0x90,note,velocity);    

  if (slot >= 0)
  {
    char buf[50];

    sprintf(buf,"/teensy1/audio/env%c/noteOff",slot+'A');
    bndl.add(buf);
    reply.add("/noteOff");
    processBundle(&bndl,reply);
  }
}


void initMIDI(void)
{
  pinMode(LED_BUILTIN,OUTPUT); // use as MIDI monitor 
  usbMIDI.setHandleNoteOff(OSCnoteOff);
  usbMIDI.setHandleNoteOn(OSCnoteOn);
  for (size_t i=0;i<COUNT_OF(allocate);i++)
    allocate[i] = AVAIL;
}


unsigned int offTime;
void updateMIDI(void)
{
  while (usbMIDI.read()) 
  {
    offTime = millis()+100;
    dbgMIDIget(); // mop up untrapped messages
  }

  if (millis() < offTime)
    digitalWrite(LED_BUILTIN,1);
  else
    digitalWrite(LED_BUILTIN,0);
}
