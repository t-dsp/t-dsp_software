/* Open Sound Control subscription library for Teensy 3.x, 4.x
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
 
#include "OSCSubscribe.h"

//=====================================================================================
/**
 * Take a copy of the src OSCMessage datum at pos, and append it to the dest OSCMessage.
 */
static OSCMessage& addCopy(OSCMessage& dest, //!< destination message
                           OSCMessage& src,  //!< source message
                           int pos)          //!< position of datum to copy
{
  char posType = src.getType(pos);
  
  switch (posType)
  {
    case 'i':
      dest.add(src.getInt(pos));
      break;

    case 'f':
      dest.add(src.getFloat(pos));
      break;

    case 'd':
      dest.add(src.getDouble(pos));
      break;
      
    case 'T':
    case 'F':
      dest.add(src.getBoolean(pos));
      break;

    case 't':
      dest.add(src.getTime(pos));
      break;

    case 's':
    {
      char* buf = (char*) alloca(src.getDataLength(pos)+1);
      if (NULL != buf)
      {
        src.getString(pos,buf);
        dest.add(buf);
      }
    }
      break;
      
    case 'b':
    {
      int len = src.getBlobLength(pos);
      uint8_t* buf = (uint8_t*) alloca(len);
      if (NULL != buf)
      {
        src.getBlob(pos,buf);
        dest.add(buf,len);
      }
    }
      break;
      
    default:
      break;
  }
  return dest;
}


//=====================================================================================
/**
 * Construct a new OSCSubscribe object.
 * Subscription interval and lifetime are expressed in milliseconds.
 */
OSCSubscribe::OSCSubscribe(subTimer_t intvl, 
						   subTimer_t lifeT,
						   char* addr,
						   OSCSubscribe* nxt)
  : interval(intvl), lifeTime(lifeT), subAddrLen(strlen(addr)), next(nxt)
  {
    nextTime = OSCSUB_NOW;    // set to fire immediately
    if (0 != lifeTime)      // unless requested to live indefinitely...
      lifeTime += nextTime; // ...set when subscription should end
	sub.empty();
    sub.setAddress(addr);   // set the OSC message address pattern used when firing
    Serial.printf("Sub: address %s length %d; next @ %d; interval %d; end %d\n",addr,subAddrLen,nextTime,interval,lifeTime);
  }


/**
 * Execute a subscription, if the time has come for it to occur.
 */
void OSCSubscribe::doSub(void (*routeFn)(OSCMessage*,OSCBundle&),
						 OSCBundle& bndl)
{
  if (OSCSUB_NOW >= nextTime)
  {
    nextTime += interval; // arrange for the subscription to fire again in the future
    
    // normally we'd fire the message off, but this is debug code:
    char* buf = (char*) alloca(subAddrLen+1);
    getMessageAddress(sub,(void*) buf,subAddrLen+1);
    Serial.printf("Fire %s at %d; next at %d; %08X, %d\n",buf,OSCSUB_NOW,nextTime, (uint32_t) buf,strlen(buf));
	
	// if we know how to fire the message, do that and add to the result bundle
	if (NULL != routeFn)
		routeFn(&sub,bndl);
  }
}


//=====================================================================================
/**
 * Create a new subscription.
 * The first three data in msg are the interval, lifetime and OSC Address Pattern of an
 * OSCMessage to fire at the given interval; any additional data are appended to the
 * periodic message. The response to the message should be sent to the client in a manner
 * defined by the application.
 */
bool OSCSubscriptionList::subscribe(OSCMessage& msg)
{ 
  bool result = false;
  size_t subAddrLen = msg.getDataLength(2)+1; // include space for terminator
  char* buf = (char*) alloca(subAddrLen);

  if (NULL != buf)
  {
    OSCSubscribe* pNewSub;
    int eod = msg.size(); // count of data in msg
    
    msg.getString(2,buf); // get target for periodic OSCMessage
    pNewSub = new OSCSubscribe(msg.getInt(0),msg.getInt(1),buf,pSubs); // create new object with timing info and pattern
    for (int i=3;i<eod;i++) // append copies of remaining data
      addCopy(pNewSub->sub,msg,i);
    pSubs = pNewSub;        // link into polling list

    result = true;
  }
  return result;
}


/**
 * Delete an existing subscription.
 * The message is similar to a subscription message, except that the interval
 * and lifetime are not needed, and only the subscription address needs to be 
 * supplied.
 */
int OSCSubscriptionList::unsubscribe(OSCMessage& msg)
{ 
  int count = 0;
  char* msgAddr;
  OSCSubscribe** pp = &pSubs;
  OSCSubscribe*  p;

  // get the address pattern of the subscription we want to delete
  msgAddr = (char*) alloca(msg.getDataLength(0)+1);
  if (NULL != msgAddr)
  {
    msg.getString(0,msgAddr);
      
    while (NULL != *pp)
    {
      p = *pp;
      if (p->sub.fullMatch(msgAddr)) // does a strcmp() first, so should work
      {
        *pp = p->next;
        delete p;
        count++;
      }
      else
        pp = &(p->next);
    }
  }

  return count;
}


/**
 * Renew an existing subscription.
 * The message is similar to a subscription message, except that only the 
 * lifetime and subscription address need to be supplied. Ensures that
 * a subscription can continue for longer while remaining in sync with its
 * original period.
 */
int OSCSubscriptionList::renew(OSCMessage& msg)
{ 
  int count = 0;
  char* msgAddr;
  OSCSubscribe** pp = &pSubs;
  OSCSubscribe*  p;

  // get the address pattern of the subscription we want to rejuvenate
  msgAddr = (char*) alloca(msg.getDataLength(1)+1);
  if (NULL != msgAddr)
  {
	subTimer_t newLifeTime = msg.getInt(0) + OSCSUB_NOW;
    msg.getString(1,msgAddr);
      
    while (NULL != *pp)
    {
      p = *pp;
      if (p->sub.fullMatch(msgAddr)) // does a strcmp() first, so should work
      {
        p->lifeTime = newLifeTime;
        count++;
      }
	  pp = &(p->next);
    }
  }

  return count;
}



/**
 * See if any subscriptions are ready to run, and execute them if so.
 * Results will be added to the supplied bundle, which must contain at least one OSCMessage
 * with its address set. If any subscriptions run, they will fill data into this message,
 * and create further messages with the same address if multiple subscriptions produce results.
 */
int OSCSubscriptionList::update(OSCBundle& bndl)
{
  int result; 
  OSCSubscribe** pp = &pSubs;
  OSCSubscribe*  p;

  while (NULL != *pp)
  {
    p = *pp;
    p->doSub(routeFn,bndl);
    if (0 != p->lifeTime && p->nextTime >= p->lifeTime)
    {
      *pp = p->next;
      delete p;
    }
    else
      pp = &(p->next);
  }

  result = bndl.size();
  if (1 == result) // may just be the default message with its address
  {
    OSCMessage* pMsg = bndl.getOSCMessage(0);
    if (0 == pMsg->size()) // any data added?
      result = 0;         // no, say no results
  }

  return result;
}


/**
 * Route an OSCMessage aimed at us to the appropriate method.
 */
void OSCSubscriptionList::route(OSCMessage& msg, int addrOff, OSCBundle& reply)
{
       if (isStaticTarget(msg,addrOff,"/addSub","iis*")) {bool ok = subscribe(msg); staticPrepareReplyResult(msg,reply).add((int)(ok?OK:NO_MEMORY));}
  else if (isStaticTarget(msg,addrOff,"/unSub", "s"))    {int cnt = unsubscribe(msg); staticPrepareReplyResult(msg,reply).add(cnt).add((int) OK);}
  else if (isStaticTarget(msg,addrOff,"/renew", "is"))   {int cnt = renew(msg); staticPrepareReplyResult(msg,reply).add(cnt).add((int) OK);}
}
