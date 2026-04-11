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

//#include "dynamic-util.h"
#include "OSCAudioBase.h"

// set this to the hardware serial port you wish to use
#define HWSERIALPORT Serial7

SLIPEncodedSerial HWSERIAL(HWSERIALPORT);

//-----------------------------------------------------------------------------------------------------------------
void setup() {
	Serial.begin(115200);
  while(!Serial)
    ;
    
  while (!(SD.begin(BUILTIN_SDCARD))) 
  {
      Serial.println("Unable to access the SD card");
      delay(500);
  }
  
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


OSCBundle* replyStack; // where reply is currently being built

//-----------------------------------------------------------------------------------------------------------------
// route messages to existing audio objects
void routeAudio(OSCMessage& msg, int addressOffset)
{
  Serial.println("audio message!");
  OSCAudioBase::routeAll(msg,addressOffset,*replyStack);
}


//-----------------------------------------------------------------------------------------------------------------
// route messages to create / modify audio objects
void routeDynamic(OSCMessage& msg, int addressOffset)
{
#if defined(SAFE_RELEASE)  
  Serial.println("dynamic objects message!");
  OSCAudioBase::routeDynamic(msg,addressOffset,*replyStack);
#else
  Serial.println("dynamic objects not available!");
#endif // defined(SAFE_RELEASE)  
}

//-----------------------------------------------------------------------------------------------------------------
// save blob to filesystem
void saveFS(OSCMessage& msg, int addressOffset)
{
  char fn[50];
  uint8_t buf[65];
  int remain = msg.getBlobLength(1);
  int idx = 0;
  OSCAudioBase::error retval = OSCAudioBase::OK;
  OSCMessage& repl = OSCAudioBase::staticPrepareReplyResult(msg,*replyStack);
  File saveFile;
  
  msg.getString(0,fn,50);
  saveFile = SD.open(fn,FILE_WRITE_BEGIN);
  if (saveFile)
  {
    Serial.print("Save ");
    while (remain > 0)
    {
      int toGet = remain>64?64:remain;
      int got = msg.getBlob(1,buf,64,idx,toGet);
      int wrote = saveFile.write(buf,got);
      buf[64] = 0;
      if (remain < 64)
        buf[remain] = 0;
      remain -= got;
      Serial.printf("<%s> @ %d: wrote %d, %d left\n",buf,idx,wrote,remain); Serial.flush();
      idx += got;
    }
    saveFile.close();
    repl.add("saved");
    Serial.printf(" (length %d) to %s\n",msg.getBlobLength(1),fn); Serial.flush();
  }
  else
  {
    retval = OSCAudioBase::NOT_FOUND;
    repl.add("failed");
  }    
  repl.add(retval);
}


//------------------------------------------------------------------------------------------------------------------------------
// retrieve blob from filesystem
// Annoyingly, CNMAT / OSC doesn't appear to be able to build a blob's content incrementally,
// so this operation has to load the entire file in one go, then create another copy of it
// in the messaging structure, resulting in heap fragmentation.
void sendFS(OSCMessage& msg, int addressOffset)
{
  char fn[50];
  uint8_t* buf;
  int remain;
  bool success = false;
  OSCAudioBase::error retval = OSCAudioBase::OK;
  OSCMessage& repl = OSCAudioBase::staticPrepareReplyResult(msg,*replyStack);
  File sendFile;
  
  msg.getString(0,fn,50);
  sendFile = SD.open(fn);
  if (sendFile)
  {
    remain = sendFile.size();
    Serial.print("Send ");
  
    if (NULL != (buf = (uint8_t*) malloc(remain+1)))
    {
      sendFile.read(buf,remain);
      repl.add(buf,remain); 
      buf[remain] = 0;
      Serial.println((char*) buf);
      free(buf);
      success = true;
    }
    else
    {
      retval = OSCAudioBase::NO_MEMORY;
      repl.add("failed");
    }
    sendFile.close();
  }
  else
  {
    retval = OSCAudioBase::NOT_FOUND;
    repl.add("failed");
  }  
  repl.add(retval);
}


//------------------------------------------------------------------------------------------------------------------------------
// Delete file from filesystem
void deleteFS(OSCMessage& msg, int addressOffset)
{
  char fn[50];
  bool success;
  OSCMessage& repl = OSCAudioBase::staticPrepareReplyResult(msg,*replyStack);
  
  msg.getString(0,fn,50);
  
  Serial.print("Delete ");

  if (true == (success = SD.remove(fn)))
    repl.add(fn); 
  else
    repl.add("failed");
  
  repl.add(success
            ?OSCAudioBase::OK
            :OSCAudioBase::NOT_FOUND);
}


//------------------------------------------------------------------------------------------------------------------------------
// Retrieve blob from filesystem and process it as an OSC-encoded packet
static int loadFSdepth = 0; // do a recursion check
void loadFS(OSCMessage& msg, int addressOffset)
{
  char fn[50];
  uint8_t* buf;
  int remain;
  OSCAudioBase::error retval = OSCAudioBase::OK;
  OSCMessage& repl = OSCAudioBase::staticPrepareReplyResult(msg,*replyStack);
  OSCBundle myReplies;
  File loadFile;

  loadFSdepth++;
  myReplies.add("/load");
  msg.getString(0,fn,50);
  loadFile = SD.open(fn);
  if (loadFile)
  {
    remain = loadFile.size();
    Serial.print("Load ");
  
    if (NULL != (buf = (uint8_t*) malloc(remain+1)))
    {
      loadFile.read(buf,remain);
      //repl.add(buf,remain); // attaches file contents as blob
      repl.add(fn); // attaches file name as string
      buf[remain] = 0;
      if ('#' == buf[0]) // got a bundle
      {
        OSCBundle bndl;
        bndl.fill(buf,remain);
        Serial.println("bundle"); Serial.flush();
        processBundle(&bndl,myReplies);
      }
      else if ('/' == buf[0]) // got a message
      {
        OSCMessage msg;
        msg.fill(buf,remain);
        Serial.println("message"); Serial.flush();
        processMessage(&msg,myReplies);      
      }
      Serial.println((char*) buf);  Serial.flush();
      free(buf);
    }
    else
    {
      retval = OSCAudioBase::NO_MEMORY;
      repl.add("failed");
    }
    loadFile.close();
  }
  else
  {
    retval = OSCAudioBase::NOT_FOUND;
    repl.add("failed");
  }

  repl.add(retval);
  
  // tack myReplies on here:
  {
    int msgN = myReplies.size(); // number of messages in reply to file execution
    for (int i=0;i<msgN;i++)
      replyStack->add(*myReplies.getOSCMessage(i));
  }
  loadFSdepth--;
}

//------------------------------------------------------------------------------------------------------------------------------
// credit for this function to manicksan:
void listFS(OSCMessage& msg, int addressOffset)
{
    char fn[50];
    OSCAudioBase::error retval = OSCAudioBase::OK;
    OSCMessage& repl = OSCAudioBase::staticPrepareReplyResult(msg,*replyStack);
    File dir;
    
    msg.getString(0,fn,50);
    dir = SD.open(fn);
    if (dir)
    {
        char *intStr = (char*)malloc(21); // to store file sizes (uint64 is 20 digits long) don't think we really need that as it's 18exabytes
        repl.add(fn);
        while (true)
        {
            File entry =  dir.openNextFile();
            if (! entry)
            {
                // no more files
                break;
            }
            if (entry.isDirectory())
            {
                repl.add("dir");
            }
            else
            {
                // files have sizes, directories do not
                repl.add("file");
                itoa(entry.size(),intStr,10);
                repl.add(intStr);
            }
            repl.add(entry.name());
            entry.close();
        }
        free(intStr);
    }
    else
    {
        repl.add(fn);
        retval = OSCAudioBase::NOT_FOUND;
        repl.add("failed");
    }  
    repl.add(retval);
}


//------------------------------------------------------------------------------------------------------------------------------
// route messages to filing system:
//   /load<s>: string s is the name of a file containing an OSC bundle which should be fed back into the OSC decoder
//   /save<s><b>: save blob b to filename s; the blob can be anything but is likely to be either JSON or OSC
//   /send<s>: retrieve a file and send it in the reply to the GUI (or other client) as a blob
//   /delete<s>: delete a file
void routeFS(OSCMessage& msg, int addressOffset)
{
  Serial.println("filesystem message!");
  if (OSCAudioBase::isStaticTarget(msg,addressOffset,"/save","sb"))
    saveFS(msg,addressOffset);
  else if (OSCAudioBase::isStaticTarget(msg,addressOffset,"/send","s"))
    sendFS(msg,addressOffset);
  else if (OSCAudioBase::isStaticTarget(msg,addressOffset,"/load","s"))
    loadFS(msg,addressOffset);
  else if (OSCAudioBase::isStaticTarget(msg,addressOffset,"/delete","s"))
    deleteFS(msg,addressOffset);
  else if (OSCAudioBase::isStaticTarget(msg,addressOffset,"/list","s"))
    listFS(msg,addressOffset);
}


//-----------------------------------------------------------------------------------------------------------------
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


//-----------------------------------------------------------------------------------------------------------------
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
    msg->route("/teensy*/fs",routeFS); // see if this object can use the message
  }
  else
    Serial.println("error in msg");
  
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
      Serial.printf("Message %d\n",i);
      processMessage(msg,reply);   
    }  
  }
  else
  {
    Serial.printf("error %d in bundle\n",(int) bndl->getError());
    
    for (int i=0;i<bndlSize;i++)
    {
      OSCMessage* msg = bndl->getOSCMessage(i); 
      Serial.printf("error %d in message %d\n",(int) msg->getError(),i);
    }
  }  
}


void sendReply(OSCBundle& reply)
{
  // for debug
  //reply.send(Serial); 
  Serial.printf("\nReply has %d messages, %d errors\n",reply.size(),reply.hasError());  

  // for real!
  HWSERIAL.beginPacket();
  reply.send(HWSERIAL); 
  HWSERIAL.endPacket();
}


//-----------------------------------------------------------------------------------------------------------------
// work with SLIP-protocol serial port:
void loop()
{
  OSCBundle bndl;
  OSCBundle reply;
  OSCMessage msg;
  long long tt = 0; //0x4546474841424344; // for debug: ABCDEFGH
  char firstCh = 0;
  int msgLen;

  Serial.print("Waiting...");
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
  
  Serial.println("processing!");
  reply.setTimetag((uint8_t*) &tt).add("/reply"); // create first message with reply address: used for all messages
  
  if ('#' == firstCh)
  {
    processBundle(&bndl,reply);
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
