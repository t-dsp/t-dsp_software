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
 *  Empty audio design for use with Audio Design Tool++
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

// set this to the hardware serial port you wish to use
#define HWSERIALPORT Serial7

SLIPEncodedSerial HWSERIAL(HWSERIALPORT);

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
  AudioMemory(50); // no idea what we'll need, so allow plenty
  //-------------------------------
  //testSanitise();
  listObjects();
}

OSCBundle* replyStack;
void routeAudio(OSCMessage& msg, int addressOffset)
{
  Serial.println("audio message!");
  OSCAudioBase::routeAll(msg,addressOffset,*replyStack);
}


void routeDynamic(OSCMessage& msg, int addressOffset)
{
#if defined(SAFE_RELEASE)  
  Serial.println("dynamic objects message!");
  OSCAudioBase::routeDynamic(msg,addressOffset,*replyStack);
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


void processMessage(OSCMessage* msg,OSCBundle& reply)
{
  char prt[200];
  OSCBundle* replyPush = replyStack;
  replyStack = &reply;
  
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
  
  replyStack = replyPush;
}


void sendReply(OSCBundle& reply)
{
  // for debug
  //reply.send(Serial); 
  Serial.printf("\nReply has %d messages\n",reply.size());  

  // for real!
  HWSERIAL.beginPacket();
  reply.send(HWSERIAL); 
  HWSERIAL.endPacket();
}

// work with SLIP-protocol serial port:
void loop()
{
  OSCBundle bndl;
  OSCBundle reply;
  OSCMessage msg;
  long long tt = 0; //0x4546474841424344; // for debug: ABCDEFGH
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

  reply.setTimetag((uint8_t*) &tt).add("/reply"); // create first message with reply address: used for all messages
  
  if ('#' == firstCh)
  {
    if (!bndl.hasError())  
    {
      int bndlSize = bndl.size();
      
      for (int i=0;i<bndlSize;i++)
      {
        OSCMessage* msg = bndl.getOSCMessage(i); 
        Serial.printf("Message %d\n",i);
        processMessage(msg,reply);   
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
    sendReply(reply);
    listObjects();
  }
  else 
  {
    if ('/' == firstCh) 
    {
      processMessage(&msg,reply);   
      sendReply(reply);
      listObjects();
    }
  }
  Serial.println();
}
