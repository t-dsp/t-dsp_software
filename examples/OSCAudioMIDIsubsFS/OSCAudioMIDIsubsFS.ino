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
 *  Includes filesystem and MIDI demo code.
 *  
 *   "C:\Program Files (x86)\Arduino\hardware\tools\arm\bin\arm-none-eabi-addr2line" -e 
 */

#include <OSCBundle.h>

#include <Audio.h>
/*
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <SerialFlash.h>
*/

//#include "dynamic-util.h"
#include "OSCAudioBase.h"

bool list_enable = false;
void listObjects(void) {listObjects(list_enable);}

#define noOSC_USE_USB_SERIAL
#if defined(OSC_USE_USB_SERIAL)
// set this to the USB serial port you wish to use
// SLIPEncodedUSBSerial edited to allow and force 
// use of second USB serial port
/*
#define USBSERIALPORT SerialUSB1
#include <SLIPEncodedUSBSerial.h>
SLIPEncodedUSBSerial HWSERIAL(USBSERIALPORT);
*/
#include <SLIPEncodedTemplateSerial.h>
SLIPEncodedUSBSerial<usb_serial2_class> HWSERIAL(SerialUSB1);
#else
// set this to the hardware serial port you wish to use
#define HWSERIALPORT Serial1
#include <SLIPEncodedSerial.h>
SLIPEncodedSerial HWSERIAL(HWSERIALPORT);
#endif // defined(OSC_USE_USB_SERIAL)

//-----------------------------------------------------------------------------------------------------------------
void setup() {
	Serial.begin(115200); 
  while(!Serial)
    ;
    
  initFS();
  initMIDI();
  initSubscribe();
  //initSynth();
  
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
  listObjects(list_enable);
  //listAllTypes();

  buildSynth();
  //testSynth();
}


OSCBundle* replyStack; // where reply is currently being built
//-----------------------------------------------------------------------------------------------------------------
// Append error code if message didn't reach a destination
void checkIfRouted(OSCBundle* reply, const char* dst)
{
  if (1 == reply->size())
  {
    OSCMessage* pMsg = reply->getOSCMessage(0);
    if (0 == pMsg->size()) // message wasn't routed...
      pMsg->add(dst).add(OSCAudioBase::NOT_ROUTED);
  }
}


//-----------------------------------------------------------------------------------------------------------------
// route messages to existing audio objects
void routeAudio(OSCMessage& msg, int addressOffset)
{
  AudioNoInterrupts();
  //Serial.println("audio message!");
  OSCAudioBase::routeAll(msg,addressOffset,*replyStack);
  AudioInterrupts();
  checkIfRouted(replyStack,"audio");
}


//-----------------------------------------------------------------------------------------------------------------
// route messages to create / modify audio objects
void routeDynamic(OSCMessage& msg, int addressOffset)
{
#if defined(SAFE_RELEASE)  
  AudioNoInterrupts();
  //Serial.println("dynamic objects message!");
  OSCAudioBase::routeDynamic(msg,addressOffset,*replyStack);
  AudioInterrupts();
#else
  Serial.println("dynamic objects not available!");
#endif // defined(SAFE_RELEASE)  
  checkIfRouted(replyStack,"dynamic");
}


//-----------------------------------------------------------------------------------------------------------------
// route messages to create / modify subscriptions
void routeSub(OSCMessage& msg, int addressOffset)
{
  routeSubscribe(msg,addressOffset,*replyStack);
  checkIfRouted(replyStack,"subscribe");
}


//-----------------------------------------------------------------------------------------------------------------
// route messages to system functions
void routeSys(OSCMessage& msg, int addressOffset)
{
  routeSystem(msg,addressOffset,*replyStack);
  checkIfRouted(replyStack,"system");
}


//-----------------------------------------------------------------------------------------------------------------
// Process a message. Because we can only extract messages from a bundle 
// by address, we have to accept a pointer rather than a reference
void processMessage(OSCMessage* msg,OSCBundle& reply)
{
  char prt[200];
  OSCBundle* replyPush = replyStack;
  replyStack = &reply;
  
  if (!msg->hasError())
  {
    msg->getAddress(prt);  
    //Serial.println(prt);
    //Serial.flush();

    // This is the key point at which the user can route incoming OSC messages
    // to whichever engines they choose to implement. The OSCAudio library
    // implements /audio and /dynamic, but /fs is implemented within this demo
    
    if (!msg->route("/teensy*/audio",routeAudio))                     // see if this object can use the message
      if (!msg->route("/teensy*/dynamic",routeDynamic))               // or this one
        if(!msg->route("/teensy*/fs",routeFS))                        // or this one
          if(!msg->route("/teensy*/subscribe",routeSub))              // or this one
            if(!msg->route("/teensy*/system",routeSys))               // or this one
              reply.getOSCMessage(0)->add(OSCAudioBase::NOT_ROUTED);  // got no takers - say so
  }
  else
  {
    Serial.println("error in msg");
    reply.getOSCMessage(0)->add(OSCAudioBase::NOT_ROUTED);
  }
  
  replyStack = replyPush;
}


void processBundle(OSCBundle* bndl,OSCBundle& reply)
{
  int bndlSize = bndl->size();

  if (!bndl->hasError())  
  {
    for (int i=0;i<bndlSize;i++)
    {
      OSCMessage* msg = bndl->getOSCMessage(i); 
      //Serial.printf("Message %d\n",i);
      processMessage(msg,reply);   
    }  
  }
  else
  {
    //Serial.printf("error %d in bundle\n",(int) bndl->getError());
    
    for (int i=0;i<bndlSize;i++)
    {
      OSCMessage* msg = bndl->getOSCMessage(i); (void) msg;
      //Serial.printf("error %d in message %d\n",(int) msg->getError(),i);
    }
  }  
}


void sendReply(OSCBundle& reply)
{
  int errCount = 0;
  // for debug
  //reply.send(Serial);
  OSCMessage* pMsg;

  checkIfRouted(&reply,"teensy");
  
  Serial.printf("\nReply has %d messages, %d OSC errors\n",reply.size(),reply.hasError()); 
  for (int i=reply.size()-1;i>=0;i--)
  {
    pMsg = reply.getOSCMessage(i);
    int last = pMsg->size()-1;
    int errv;
    if (last > 0 && pMsg->isInt(last) && (errv = pMsg->getInt(last)) != 0)
    {
      errCount++;
      Serial.printf("%d ",errv);
    }
  }
  if (errCount > 0)
    Serial.printf(": %d error flags\n",errCount);

  // for real!
  HWSERIAL.beginPacket();
  reply.send(HWSERIAL); 
  HWSERIAL.endPacket();
}


//-----------------------------------------------------------------------------------------------------------------
// work with SLIP-protocol serial port:
void updateOSC()
{
  static enum {boot,reading,processing} state = boot;
  static OSCBundle* bndl;
  static OSCBundle reply;
  static OSCMessage msg;
  long long tt = 0; //0x4546474841424344; // for debug: ABCDEFGH
  static char firstCh = 0;
  int msgLen;

  switch (state)
  {
    case boot:
      Serial.print("Waiting...");
      bndl = new OSCBundle; // because empty() doesn't work...
      //bndl->empty();
      reply.empty();
      msg.empty();
      firstCh = 0;
      state = reading;
      break;
    
    case reading:
      if (!HWSERIAL.endofPacket())
      {    
        msgLen = HWSERIAL.available(); // only ever returns 0 or 1, actually
        while (msgLen--)
        {
          char c = HWSERIAL.read();            
          // figure out if it's a message or a bundle
          if (0 == firstCh)
            firstCh = c;
          if ('#' == firstCh)
            bndl->fill((uint8_t) c); // simple messages should result in a 1-message "bundle", but don't
          else
            msg.fill((uint8_t) c); // so process them specifically
        }
      }
      else
      {
        state = processing;
      }
      break;

    case processing:  
      Serial.println("processing!");
      reply.setTimetag((uint8_t*) &tt).add("/reply"); // create first message with reply address: used for all messages
      
      if ('#' == firstCh)
      {
        processBundle(bndl,reply);
        sendReply(reply);
        listObjects(list_enable);
      }
      else 
      {
        if ('/' == firstCh) 
        {
          processMessage(&msg,reply);   
          sendReply(reply);
          listObjects(list_enable);
        }
      }
      Serial.println();
      delete bndl;
      state = boot;
      break;
  }
}

//-----------------------------------------------------------------------------------------------------------------
// Simplified loop: co-operative multi-tasking, sort of
void loop(void)
{
  updateOSC();
  updateMIDI();  
  updateSubscribe();
}
