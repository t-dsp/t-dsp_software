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

/* 
 *  Play with OSC routing. Test input comes from OSCAudioSend.py etc.
 *  
 *   "C:\Program Files (x86)\Arduino\hardware\tools\arm\bin\arm-none-eabi-addr2line" -e 
 */

#include <OSCBundle.h>
#include <SLIPEncodedSerial.h>


#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <SerialFlash.h>

#include "OSCAudioBase.h"

// GUItool: begin automatically generated code
//AudioSynthWaveform       waveform1;      //xy=654,472
OSCAudioSynthWaveform   waveform1("waveform1");
OSCAudioMixer4          mixer1("mixer1");
AudioOutputI2S           i2s1;           //xy=977,476
AudioConnection          patchCord1(waveform1, 0, i2s1, 0);
AudioConnection          patchCord2(waveform1, 0, mixer1, 1);
AudioConnection          patchCord3(mixer1, 0, i2s1, 1);
OSCAudioControlSGTL5000     sgtl5000_1("sgtl5000");     //xy=977,519
// GUItool: end automatically generated code



#define USE_SLIP

// set this to the hardware serial port you wish to use
#define HWSERIALPORT Serial7

#if defined(USE_SLIP)
SLIPEncodedSerial HWSERIAL(HWSERIALPORT);
#else
#define HWSERIAL HWSERIALPORT
#endif // defined(USE_SLIP)

void setup() {
	Serial.begin(115200);
  while(!Serial)
    ;

  if (CrashReport)
  {
    Serial.println(CrashReport);
    CrashReport.clear();
  }
	HWSERIAL.begin(115200);
  HWSERIAL.setTimeout(100);

  //-------------------------------
  AudioMemory(10);
  sgtl5000_1.enable();
  sgtl5000_1.volume(0.1);
  //-------------------------------
  //testSanitise();
  listObjects();
}

OSCBundle reply;
void routeAudio(OSCMessage& msg, int addressOffset)
{
  Serial.println("audio message!");
  OSCAudioBase::routeAll(msg,addressOffset,reply);
}


void routeDynamic(OSCMessage& msg, int addressOffset)
{
#if defined(SAFE_RELEASE)  
  Serial.println("dynamic objects message!");
  OSCAudioBase::routeDynamic(msg,addressOffset,reply);
#else
  Serial.println("dynamic objects not available!");
#endif // defined(SAFE_RELEASE)  
}


void listObjects(void)
{
  OSCAudioBase* obj=OSCAudioBase::getFirst();

  while (NULL != obj)
  {
    Serial.printf("%s is at %08X\n",obj->name,(uint32_t) obj);
    Serial.flush();
    obj = obj->getNext();
  }
}


void testSanitise()
{
  char buf[20];
  OSCAudioBase::sanitise("Hello world!",buf); Serial.println(buf);  
  OSCAudioBase::sanitise("[Hello world!]",buf); Serial.println(buf);  
  OSCAudioBase::trimUnderscores(buf,buf); Serial.println(buf);  
  OSCAudioBase::sanitise("*[H]e{}llo#, world!?/",buf); Serial.println(buf);  
  OSCAudioBase::trimUnderscores(buf,buf); Serial.println(buf);  
  OSCAudioBase::sanitise("#*,/? []{}",buf); Serial.println(buf);  
  OSCAudioBase::trimUnderscores(buf,buf); Serial.printf("<%s>\n",buf);  
}


void processMessage(OSCMessage* msg)
{
  char prt[200];
  
  if (!msg->hasError())
  {
    msg->getAddress(prt);  
    Serial.println(prt);
    Serial.flush();
  
    msg->route("/teensy*/audio",routeAudio); // see if this object can use the message
    msg->route("/teensy*/dynamic",routeDynamic); // see if this object can use the message
  }
  else
    Serial.println("error in msg");
}


// work with SLIP-protocol serial port:
void loop()
{
  OSCBundle bndl;
  OSCMessage msg;
  char firstCh = 0;
  int msgLen;
  
  while (!HWSERIAL.endofPacket())
  {
    
    msgLen = HWSERIAL.available();
    while (msgLen--)
    {
      char c = HWSERIAL.read();
      // figure out if it's a message or a bundle
      if (0 == firstCh)
        firstCh = c;
      if ('#' == firstCh)
        bndl.fill((uint8_t) c); // simple messages should result in a 1-message "bundle", but don't
      else
        msg.fill((uint8_t) c); // so process them specifically
    }
  }
  
  reply.empty();       // keep reply bundle empty - we don't use it in this sketch...
  reply.add("/reply"); // ...except OSC library will crash if we don't put something in

  if ('#' == firstCh)
  {
    if (!bndl.hasError())  
    {
      int bndlSize = bndl.size();
      
      for (int i=0;i<bndlSize;i++)
      {
        OSCMessage* msg = bndl.getOSCMessage(i); 
        Serial.printf("Message %d\n",i);
        processMessage(msg);   
      }  
    }
    else
    {
      Serial.printf("error %d in bundle\n",(int) bndl.getError());
      int bndlSize = bndl.size();
      
      for (int i=0;i<bndlSize;i++)
      {
        OSCMessage* msg = bndl.getOSCMessage(i); 
        Serial.printf("error %d in message %d\n",(int) msg->getError(),i);
      }
    }
    listObjects();
  }
  else 
  {
    if ('/' == firstCh) 
    {
      processMessage(&msg);   
      listObjects();
    }
  }
  Serial.println();
}
