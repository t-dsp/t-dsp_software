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
 
#include "OSCAudioUseMIDI.h"

#define RESTART_ADDR 0xE000ED0C
#define READ_RESTART() (*(volatile uint32_t *)RESTART_ADDR)
#define WRITE_RESTART(val) ((*(volatile uint32_t *)RESTART_ADDR) = (val))


//------------------------------------------------------------------------------------------------------------------------------
extern uint8_t _estack;
void printStack(OSCMessage& repl)
{
  int a = ((uint32_t) &_estack) - ((uint32_t) &a);
  Serial.printf("Stack used %lu bytes\n",a);  
  repl.add(a);
}

//------------------------------------------------------------------------------------------------------------------------------
#if defined(ARDUINO_TEENSY41) || defined(ARDUINO_TEENSY40)
extern uint8_t _heap_start,_heap_end;
#define HEAP_START &_heap_start
#define HEAP_END   &_heap_end
#else
extern uint8_t __bss_end;
#define HEAP_START &__bss_end
#define HEAP_END   (&_estack - 4096) // guess - allow 4k stack
#endif

void printHeapX(int* x, int* y)
{
  char* a = (char*) malloc(10000);
  int u = ((uint32_t) a) - ((uint32_t) HEAP_START);
  int f = ((uint32_t) HEAP_END) - ((uint32_t) a);
  Serial.printf("Heap used %lu bytes; free %lu bytes\n",u,f);  
  free(a);
  *x = u; *y = f;
}

void printHeap(OSCMessage& repl)
{
  int u,f;
  printHeapX(&u,&f);
  repl.add(u).add(f);
}

//------------------------------------------------------------------------------------------------------------------------------
void printAudioCurrent(OSCMessage& repl)
{
  int blocks = AudioMemoryUsage();
  float cpu = AudioProcessorUsage();
  Serial.printf("Memory used %d blocks; CPU %f\n",blocks,cpu);  
  repl.add(blocks).add(cpu);
}

//------------------------------------------------------------------------------------------------------------------------------
void printAudioMax(OSCMessage& repl,bool res = false)
{
  int blocks = AudioMemoryUsageMax();
  float cpu = AudioProcessorUsageMax();
  Serial.printf("Max memory used %d blocks; max CPU %.2f%s\n",blocks,cpu,res?" reset":"");  
  repl.add(blocks).add(cpu);

  if (res)
  {
    AudioMemoryUsageMaxReset();
    AudioProcessorUsageMaxReset();
  }
}


//------------------------------------------------------------------------------------------------------------------------------
void setDebug(OSCMessage& msg,OSCMessage& repl)
{
  int which = msg.getInt(0),value = msg.getInt(1);

  (void) value; // may not use it
  switch (which)
  {
    case 1:
      list_enable = !list_enable;
      break;
  }
}


//------------------------------------------------------------------------------------------------------------------------------
// route messages to system:
void routeSystem(OSCMessage& msg, int addressOffset,OSCBundle& reply)
{
   OSCMessage& repl = OSCAudioBase::staticPrepareReplyResult(msg,*replyStack);
   
  Serial.println("system message!"); Serial.flush();

  // Board reset
  if (OSCAudioBase::isStaticTarget(msg,addressOffset,"/reset",NULL))
    WRITE_RESTART(0x5FA0004);

  // memory and CPU information
  else if (OSCAudioBase::isStaticTarget(msg,addressOffset,"/stack",NULL))
    printStack(repl);
  else if (OSCAudioBase::isStaticTarget(msg,addressOffset,"/heap",NULL))
    printHeap(repl);
  else if (OSCAudioBase::isStaticTarget(msg,addressOffset,"/usage",NULL))
    printAudioCurrent(repl);
  else if (OSCAudioBase::isStaticTarget(msg,addressOffset,"/max","*"))
  {
    bool res = false;
    if (msg.size() > 0)
      res = msg.getBoolean(0);
    printAudioMax(repl,res);
  }

  // trigger special debug modes
  else if (OSCAudioBase::isStaticTarget(msg,addressOffset,"/debug","ii"))
    setDebug(msg,repl);
  else if (OSCAudioBase::isStaticTarget(msg,addressOffset,"/listObjects",NULL))
    listObjects(true);

  // demo synth building in code
  else if (OSCAudioBase::isStaticTarget(msg,addressOffset,"/buildSynth",NULL))
    buildSynth();
  else if (OSCAudioBase::isStaticTarget(msg,addressOffset,"/destroySynth",NULL))
    destroySynth();

  repl.add(0);
}
//-----------------------------------------------------------------------------------------------------------------
