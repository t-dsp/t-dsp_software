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

#include "OSCUtils.h"

//-------------------------------------------------------------------------------------------------------
static void dbgPrt(OSCMessage& msg, int addressOffset)
{
	char prt[100];
	(void) dbgPrt; // avoid warning
	msg.getAddress(prt,addressOffset);

	Serial.println(addressOffset);
	Serial.println(prt);
	Serial.println(msg.size());
	Serial.println(); 
}


/**
 * Go to insane lengths to find out address length, because library writers were lazy.
 * GRRR.
 */
size_t OSCUtils::getMessageAddressLen(OSCMessage& msg)
{
	/*
	char* buf = alloca(msg.bytes()); // get stupid amount of stack memory
	msg.getAddress(buf); 			 // guaranteed enough, though
	return strlen(buf)+1; // add space to store 0 terminator
	*/
	
	return msg.getAddressLength()+1;
}


/*
 * Copy message Address Pattern into pre-allocated memory buffer.
 * The buffer pointer MUST have enough space for the pattern, but may be NULL (e.g. if the 
 * allocation failed) in which case the pattern is not copied. Note that OSCMessage::getAddress()
 * does not do the NULL check so is unsafe to use. And the length-protected version is undocumented.
 * \return pointer to buffer, cast to char*
 */
char* OSCUtils::getMessageAddress(OSCMessage& msg, 	//!< message to extract string from
								  void* vbuf,		//!< big enough buffer, or NULL
								  int len,			//!< length of buffer
								  int offset)		//!< offset into address pattern  (default 0)
{
	char* buf = (char*) vbuf;
	
	if (NULL != buf) // length must be OK, no further check needed
	{
		msg.getAddress(buf,offset,len); 
		OSC_SPTF("Address pattern: %s\n",buf);
	}
	
	return buf;
}


//-------------------------------------------------------------------------------------------------------
/**
 * Add to the reply bundle as a result of having executed an OSC message routed to us.
 */
void OSCUtils::addReplyExecuted(OSCMessage& msg, int addressOffset, OSCBundle& reply, char* name) 
{
	addReplyResult(msg,addressOffset,reply,true, name); // add a "true" bool to the response, because we did the method
}


/**
 * Prepare the initial part of a reply to a message routed to us.
 * Fills in the standard information generated for any successful routing.
 * \return reference to the OSCMessage, ready to add any extra information, dependent on the method called
 */
OSCMessage& OSCUtils::staticPrepareReplyResult(OSCMessage& msg, 	//!< the received message
											   OSCBundle&  reply) 	//!< the bundle that will become the reply
{
	int msgCount = reply.size(); // number of messages in bundle
	OSCMessage* pLastMsg = reply.getOSCMessage(msgCount-1); // point to last message in reply bundle
	int dataCount = pLastMsg->size(); // how many pieces of data are in that?
	size_t addrL = getMessageAddressLen(msg);
	char* replyAddress = getMessageAddress(*pLastMsg,alloca(addrL),addrL);	
	char* buf = getMessageAddress(msg,alloca(addrL),addrL);
	
	if (0 != dataCount) // message already in use...
		pLastMsg = &reply.add(replyAddress); // ... make ourselves a new one
		
	// start composing our reply:
	pLastMsg->add(buf);				// which address the routed message was destined for
	
	return *pLastMsg;
}


/**
 * Prepare the initial part of a reply to a message routed to us.
 * Fills in the standard information generated for any successful routing.
 * \return reference to the OSCMessage, ready to add any extra information, dependent on the method called
 */
OSCMessage& OSCUtils::prepareReplyResult(OSCMessage& msg, 	//!< the received message
										 OSCBundle& reply, 	//!< the bundle that will become the reply
										 char* name)		//!< name of object that caught action, or NULL
{
#if defined(OSC_DEBUG_PRINT)	
	size_t addrL = getMessageAddressLen(msg);
	char* buf = getMessageAddress(msg,alloca(addrL),addrL);
	OSC_SPTF("%s executed %s; result was ",(NULL != name)?(name+1):"<no-name>",buf);
#endif // defined(OSC_DEBUG_PRINT)	
	
	OSCMessage& rep = staticPrepareReplyResult(msg,reply); // add which element caught the routed message
	if (NULL != name)
		rep.add(name+1);
	
	return rep;
}


// Despatch function overloaded with the various reply types we might append to the standard information
void OSCUtils::addReplyResult(OSCMessage& msg, int addressOffset, OSCBundle& reply, bool v, char* name, error ret) { prepareReplyResult(msg, reply, name).add(v).add(ret); OSC_SPLN(v?"true":"false"); }
void OSCUtils::addReplyResult(OSCMessage& msg, int addressOffset, OSCBundle& reply, float v, char* name) { prepareReplyResult(msg, reply, name).add(v).add(OK); OSC_SPLN(v); }
void OSCUtils::addReplyResult(OSCMessage& msg, int addressOffset, OSCBundle& reply, int32_t v, char* name) { prepareReplyResult(msg, reply, name).add(v).add(OK); OSC_SPLN(v); }
void OSCUtils::addReplyResult(OSCMessage& msg, int addressOffset, OSCBundle& reply, uint32_t v, char* name) { prepareReplyResult(msg, reply, name).add((unsigned int)v).add(OK); OSC_SPLN(v); }
void OSCUtils::addReplyResult(OSCMessage& msg, int addressOffset, OSCBundle& reply, uint8_t v, char* name) { prepareReplyResult(msg, reply, name).add(v).add(OK); OSC_SPLN(v); }
void OSCUtils::addReplyResult(OSCMessage& msg, int addressOffset, OSCBundle& reply, uint16_t v, char* name) { prepareReplyResult(msg, reply, name).add(v).add(OK); OSC_SPLN(v); }



//=======================================================================================================

