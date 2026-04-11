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
 
// This file shows an example of building a synth at runtime, without
// any dependence on OSC messages. This works with the original static
// Audio library, even though it uses "new" : DO NOT delete it! 

#include "OSCAudioBase.h"

// Non-OSC version of the Voice1 class
class Voice1 
{
public:
    AudioSynthWaveform               wav;
    AudioSynthWaveform               wav2;
    AudioMixer4                      mixer;
    AudioEffectEnvelope              env;
    AudioConnection                  *patchCord[3]; // total patchCordCount:3 including array typed ones.

    Voice1() { // constructor (this is called when class-object is created)
        int pci = 0; // used only for adding new patchcords

        patchCord[pci++] = new AudioConnection(wav, 0, mixer, 0);
        patchCord[pci++] = new AudioConnection(wav2, 0, mixer, 1);
        patchCord[pci++] = new AudioConnection(mixer, 0, env, 0);
        
    }
    
    ~Voice1()  // destructor (this is called when the class-object is deleted)
    {
      for (int i = 0; i < 3; i++) {
        patchCord[i]->disconnect(); // don't actually need this!
        delete patchCord[i];
      }
    }
};


// OSC version of the Voice1 class
class OSCVoice1grp : public OSCAudioGroup
{
public:
    // the actual voice elements we want to group together
    OSCAudioSynthWaveform&  wav;
    OSCAudioSynthWaveform&  wav2;
    OSCAudioMixer4&         mixer;
    OSCAudioEffectEnvelope& env;

    // internal patch cords
    // total patchCordCount:3 including array typed ones.
    OSCAudioConnection*     patchCord[3];
    //OSCAudioConnection *pc1,*pc2,*pc3;

    // create as sub-group
    OSCVoice1grp(const char* _name,OSCAudioGroup* parent) : // constructor 
      OSCAudioGroup(_name,parent), // construct our base class instance
      wav(*new OSCAudioSynthWaveform{"wav",*this}),
      wav2(*new OSCAudioSynthWaveform{"wav2",*this}),
      mixer(*new OSCAudioMixer4{"mixer",*this}),
      env(*new OSCAudioEffectEnvelope{"env",*this})
      /*
     ,pc1(new OSCAudioConnection{"wav_mixer", *this, wav,   0, mixer, 0}),
      pc2(new OSCAudioConnection{"wav2_mixer",*this, wav2,  0, mixer, 1}),
      pc3(new OSCAudioConnection{"mixer_env", *this, mixer, 0, env,   0})                
      */
    { 
      
        int pci = 0; // used only for adding new patchcords
        OSCAudioGroup& grp = *this;

        patchCord[pci++] = new OSCAudioConnection("wav_mixer", grp, wav,   0, mixer, 0);
        patchCord[pci++] = new OSCAudioConnection("wav2_mixer",grp, wav2,  0, mixer, 1);
        patchCord[pci++] = new OSCAudioConnection("mixer_env", grp, mixer, 0, env,   0);                
      
    } 

    // create at root
    OSCVoice1grp(const char* _name) : OSCVoice1grp(_name,NULL) {}

    // no destructor needed, the base OSCAudioGroup   
    // destroys its members when it's destroyed
};


class OSCMixAndOutput // only bother with the ungrouped version for now
{
public:
    OSCAudioOutputAnalogStereo&          dacs;
    OSCAudioMixerStereo&                 mixerStereo;
    OSCAudioOutputI2S&                   i2s;
    OSCAudioControlSGTL5000& sgtl5000;
    OSCAudioConnection                  *patchCord[4]; // total patchCordCount:4 including array typed ones.

    OSCMixAndOutput(unsigned char nch=6) :
      dacs(*new OSCAudioOutputAnalogStereo{"dacs"}),
      mixerStereo(*new OSCAudioMixerStereo{"mixer",nch}),
      i2s(*new OSCAudioOutputI2S{"i2s"}),
      sgtl5000(*new OSCAudioControlSGTL5000{"sgtl5000"})
    { // constructor (this is called when class-object is created)
        int pci = 0; // used only for adding new patchcords

        patchCord[pci++] = new OSCAudioConnection("mix_dacs_L", mixerStereo, 0, dacs, 0);
        patchCord[pci++] = new OSCAudioConnection("mix_i2s_L",  mixerStereo, 0, i2s, 0);
        patchCord[pci++] = new OSCAudioConnection("mix_dacs_R", mixerStereo, 1, dacs, 1);
        patchCord[pci++] = new OSCAudioConnection("mix_i2s_R",  mixerStereo, 1, i2s, 1);
        
    }

    ~OSCMixAndOutput() { // destructor (this is called when the class-object is deleted)
        for (int i = 0; i < 4; i++) {
            patchCord[i]->disconnect(); // don't actually need this!
            delete patchCord[i];
        }
        delete &dacs;
        delete &mixerStereo;
        delete &i2s;
        delete &sgtl5000;
        Serial.println("OSCMixAndOutput dtor done");
    }
};

// Build the synth. Note that we would "lose track" of everything on
// exit from the function, except that the OSCAudio system can 
// find the elements for us. Or just store a couple of pointers...
#define XVOICES 6
OSCMixAndOutput* mixo;
OSCAudioGroup* voice1;
void buildSynth(void)
{
  mixo = new OSCMixAndOutput{XVOICES};
  voice1 = new OSCAudioGroup{"voice1",nullptr};
  
  for (int i=0;i<XVOICES;i++)
  {
    char grpName[50],pcName[20];

    // Create new voice instance.
    // It is a sub-member of the voice1 group, so will be
    // destroyed when we delete voice1.
    sprintf(grpName,"i%d",i);
    OSCVoice1grp* vi = new OSCVoice1grp{grpName,voice1};
    vi->mixer.gain(0,0.5);
    vi->mixer.gain(1,0.5);
    
    if (NULL != mixo)
    {
      sprintf(pcName,"i%d_mixer",i);
      // Make a connection from a voice instance to the output mixer.
      // We make it a group member of the voice instance, so it will be destroyed
      // when the instance is destroyed, and we don't need to keep track of it.
      OSCAudioConnection* pc = new OSCAudioConnection{pcName,*vi,vi->env,0,mixo->mixerStereo,(uint8_t) i};
      (void) pc;

      mixo->mixerStereo.pan(i,(2.0f*i/(XVOICES - 1)-1.0f)*0.9f);
    }
  }

  if (NULL != mixo)
  {
    mixo->mixerStereo.gain(0.16f);
    mixo->sgtl5000.enable();
    mixo->sgtl5000.volume(0.5);
    mixo->sgtl5000.lineOutLevel(14); 
  }
}


// Dismantle the synth
void destroySynth(void)
{
  AudioNoInterrupts();
  if (NULL != mixo)
  {
    delete mixo;
    mixo = NULL;
    Serial.println("mixo is no more!"); Serial.flush();
  }
  if (NULL != voice1)
  {
    delete voice1;
    voice1 = NULL;
    Serial.println("voice1 has left the building"); Serial.flush();
  }
  AudioInterrupts();
}


void testSynth()
{
  int d;
  
  for (int i = 0;i<10;i++)
  {
    printHeapX(&d,&d); zapHeap(); memDump((char*) 0x20216D8C,1024);
    destroySynth();
    printHeapX(&d,&d); zapHeap(); memDump((char*) 0x20216D8C,1024);
    buildSynth();
  }
  printHeapX(&d,&d);
}
