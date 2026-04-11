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
 
// This file shows an example of the use of the OSCSubscription mechanism
 
 #include <OSCSubscribe.h>

static OSCSubscriptionList* pSubList; // current subscription list
static OSCBundle* pSubBundle;         // where subscription result messages will be placed

/**
 * Initialise the subscription engine
 */
void initSubscribe(void)
{
  long long tt = 0;
  
  pSubList = new OSCSubscriptionList(processMessage);   //make an instance, using processMessage() when a subscription fires
  pSubBundle = new OSCBundle;
  pSubBundle->setTimetag((uint8_t*) &tt).add("/subEvt");
}


/**
 * This function must be called regularly to fire subscriptions
 * in a timely manner. If a subscription is set to fire every
 * 97ms and this is only called every 5ms, then the actual interval
 * could range from 97 to 102ms.
 */
void updateSubscribe(void)
{
  OSCMessage* pMsg;

  // poll subscriptions seeing if any need to generate an event
  pSubList->update(*pSubBundle);

  pMsg = pSubBundle->getOSCMessage(0);
  if (pMsg->size() > 0) // the update caused some activity
  {
    long long tt = 0;

    sendReply(*pSubBundle); // tell the outside world what happened
    
    // prepare for next subscription event: empty the results bundle
    pSubBundle->empty();
    pSubBundle->setTimetag((uint8_t*) &tt).add("/subEvt");
  }
}


// Route any OSC messages to the subscription list.
// We implement methods to create or delete subscriptions.
void routeSubscribe(OSCMessage& msg, int addressOffset, OSCBundle& reply)
{
  //Serial.println("subscription engine message!");
  pSubList->route(msg,addressOffset,reply);
}
