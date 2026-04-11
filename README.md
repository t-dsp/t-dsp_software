# OSCAudio
Open Sound Control layer on top of Teensy Audio library
This library provides two fundamental layers on top of the Dynamic Audio Library (currently a fork of the original Teensy Audio library, though there is some indication that it could become mainstream at some point). Both build on the OSC  library already shipped as part of Teensyduino [note to self: is the shipped one sufficiently up to date?] and provide
- control of existing audio objects via OSC
- dynamic changes to the toplogy of the audio design, without needing a complete re-compile

The examples show these in action, along with simple filesystem access and MIDI mapping; these latter are not part of the core functionality, but may serve as inspiration for more fully-fledged projects.

## OSC
A complete description of OSC is beyond the scope of this ReadMe: please refer to https://github.com/CNMAT/OSC and the API and specification documentation linked from there for full details. Essentially OSC is a standard for packing information along with a destination, such that it can be interpreted and acted upon at that destination. The transport of the information is also not part of the specification: the examples use messages which arrive via a serial port, from the Teensy filesystem, or are internally generated from MIDI data.
 
Routing of messages depends upon their address. This will be of the form **/path/to/destination/method**: at any point in the delivery process part of the address may have been "consumed", with the remainder forwarded to the next level until something executes the final "method" using the attached parameters. Within the examples we have used /teensy1 as the top level, thus acknowledging that future systems might have multiple Teensys, with messages broadcast via Ethernet or similar.

Parameters to the methods are of types integer, float, string or blob. In the description below these will be shown as type codes i, f, s or b, or values will be shown in angle brackets, e.g. <220.0> is of type f. Optional parameters will be shown in square brackets, e.g. **/dynamic/crOb,s[s]** denotes that the dynamic createObject method may take one or two strings as its parameters.

### Internal routing
The two essential layers implemented by the OSCAudio library are named /audio and /dynamic. Whilst they could in principle be changed by the application writer, these names are built in to a separate development of the Audio System Design++ tool (https://manicken.github.io/#, henceforth called GUI++ for brevity) which provides an excellent way to make use of OSCAudio.
#### /audio
Invoke by calling **OSCAudioBase::routeDynamic(OSCMessage& msg,int addressOffset,OSCBundle& reply)**

This provides control of existing audio objects. For example, assuming you have an AudioSynthWaveform object called "wav", the message **/teensy1/audio/wav/begin<0.5><220.0><0>** will cause it to output a 220Hz sine wave at 50% amplitude. Almost every function for every audio object is available, and uses the exact parameters documented in the GUI++ help. This is important to remember when constructing your own messages, as the parameter types *must* be correct for a method to be invoked. **msg("/teensy1/audio/wav/begin").add(0.5).add(220.0f).add(0)** should work, but **msg("/teensy1/audio/wav/begin").add(0.5).add(220).add(0)** won't, because the frequency is an integer, not a float.

See [here](./FunctionList.md) for a list of available methods which act on audio objects.

Some additional /audio methods are used to manipulate connection objects:
- **/c**onnect,ss: create an audio connection; first parameter is the source; second is the destination; uses port 0 in both cases
- **/c**onnect,sisi: create an audio connection; first two parameters are the source and output port; second two are the destination and input port
- **/d**isonnect: disconnect from source and destination (leaves the connection object in existence)


#### /dynamic
This allows manipulation of the audio system's topology by creating and destroying objects and connections. Methods available are:
- **crCo**nnection,s[s]: create a connection; first parameter is the name; optional second is the group
- **crOb**ject,ss[s]: create an audio object; first parameter is the type; second is the name; optional third is the group
- **crGr**oup,ss: create a group; first parameter is the name; second is the parent group - use "/" to create at the root
- **deOb**,s: destroy object; parameter is object name (audio object, connection or group)
- **ren**ame,ss: rename object; first parameter is object name; second is new name
- **clearAll**: remove all dynamic objects

For example, **/teensy1/dynamic/crOb\<AudioSynthWaveform\>\<wav\>** will create a new AudioSynthWaveform object called "wav", at the root level of your design. Here's a longer example which creates and connects a waveform and an output device, and starts an audible 220Hz tone:
~~~
/teensy1/dynamic/crOb<AudioSynthWaveform><wav>
/teensy1/dynamic/crOb<AudioOutputI2S><i2s>
/teensy1/dynamic/crOb<AudioControlSGTL5000><sgtl5000>
/teensy1/dynamic/crCo<wav_i2sL>
/teensy1/dynamic/crCo<wav_i2sR>
/teensy1/audio/wav_i2sL/co<wav><i2s>
/teensy1/audio/wav_i2sR/co<wav>0<i2s><1>
/teensy1/audio/sgtl5000/enable
/teensy1/audio/sgtl5000/vol*<0.5>
/teensy1/audio/wav/begin<0.1><220.0><0>
~~~
[the vol* seems necessary, probably a bug in the library somewhere!]

An experimental extension to **crOb** is implemented for special variable-width mixer objects *only*: a final integer parameter may be appended, which denotes the number of inputs available. 

Where it makes sense, address pattern matching may be used to affect multiple objects. For example, say we start with a design fragment such as this, which creates the basis for a 4-voice synth:
~~~
/teensy1/dynamic/crGr</voice1></>
/teensy1/dynamic/crGr</i0></voice1>
/teensy1/dynamic/crGr</i1></voice1>
/teensy1/dynamic/crGr</i2></voice1>
/teensy1/dynamic/crGr</i3></voice1>
~~~
We can then execute **/teensy1/dynamic/crOb<AudioSynthWaveform><wav></voice1/i\*>** which will create four AudioSynthWaveform objects, each called wav, in the four sub-groups of /voice, /i0 to i3. **/teensy1/dynamic/ren</voice1/i\*/wav><wav1>** would rename them all to wav1, and **/teensy1/dynamic/deOb</voice1/i\*/wav1>** would then delete them all.

Deleting a group will delete all its objects and connections.

## Efficiency considerations
### Code space
Because the OSCAudio library has to contain a copy of the code for *every possible* audio object, just in case an instance *may* be created at run-time, the compiled code tends to be quite large. It is possible to improve the situation by making a copy of the Audio library (e.g. as found at ...Arduino\hardware\teensy\avr\libraries\Audio) into your "local libraries" folder. Check that this is found correctly - the Arduino IDE outputs a line like *Using library Audio at version 1.3 in folder: <path>* towards the end of its compilation output. Once this is working, you can delete the .cpp and .h of any object type you will never use; you must also delete the corresponding #include entry in Audio.h. Having done this the objects will no longer be compiled in, with a corresponding increase in available code space.

**Note to audio object developers:** this mechanism relies on the presence of a uniquely named include guard in the object's header file. Please do not use **#pragma once**! 
# OSCSubscribe
The OSCSubscribe library is included as part of OSCAudio, though technically it can be used entirely separately. It provides a mechanism to "fire" a number of periodic OSCMessages into your system, collate the results, and send the results out to the client(s). As much of its operation is application-specific, the programmer has to provide some of the code to interface with their environment.
## OSCSubscription object
Use **OSCSubscription(void (\*routeFunction)(OSCMessage\*,OSCBundle&))** to instantiate a new subscription object. The required **routeFunction()** parameter will be called whenever a subscription fires: the OSCMessage parameter will be one of the OSC messages that has been "subscribed", and the OSCBundle is used to collate any responses.
### subscribe(OSCMessage&)
Adds a subscription to an instance of an OSCSubscription object. The passed OSCMessage must have *at least* the parameters **iis**:
* subscription interval (milliseconds)
* subscription lifetime (milliseconds); 0 means run until explicitly unsubscribed
* Address Pattern of periodically fired message

Any additional parameters supplied will be appended to the periodic OSCMessage.
Once the subscription lifetime has expired, the subscription will be deleted from the list, and a new subscription must be generated if it is required to continue to monitor the object.

### renew(OSCMessage&)
Renews an existing subscription, provided its lifetime hasn't already expired. The advantage of this is that the interval timer continues in sync, so a steady flow of results is maintained, compared to the possiblility of an uneven flow if a subscription expires and is re-created.

The passed OSCMessage must have the parameters **is**:
* subscription lifetime (milliseconds); 0 means run until explicitly unsubscribed
* Address Pattern of periodically fired message

### unsubscribe(OSCMessage&)
Deletes a subscription. The OSCMessage must have a string parameter which matches an existing subscription, which will be removed from the subscription list.

### update(OSCBundle&)
This function should be called periodically for every OSCSubscription object instantiated in your system: it will poll all subscriptions attached to the object, and for every one whose interval has passed, the routeFunction() originally defined will be called using the subscription's OSCMessage (see **subscribe()**) and the supplied OSCBundle parameter. The intention is that whenever the OSCMessage is successfully routed to a method, the target will append to the result OSCBundle any information generated by the method. See below for the expected result OSCBundle format

### route(OSCMessage&, int, OSCBundle&)
This routing function forwards a [partially used] message to the OSCSubscription router, which currently implements three methods:
- **addSub**,iis[*]: add a new subscription, according to the rules for subscribe() above
- **unSub**,s: delete a subscription, according to the rules for unsubscribe() above
- **renew**,is: renew a subscription, according to the rules for renew() above

## OSCBundle format for results
The current implementation pre-loads the OSCBundle passed to **OSCSubscription::update()** with a zero timestamp and a single OSCMessage with its address pattern pre-set. The address pattern used at the moment is "subEvt", but this can be changed if your application requires it.

When your routeFunction() produces results to be added to the bundle, it should inspect the *last* message in the bundle: if it has no data in it, then the result data should be appended to that message using **OSCMessage::add()**. If it does have data, a new OSCMessage with the *same address* and the new result data should be added to the bundle.

On return from **OSCSubscription::update()**, if the results bundle still has only one message with no data, then no subscriptions fired and no further action is needed. Otherwise, your application should take steps to use **OSCBundle::send()** to send the bundle to any recipient(s) you have implemented. 

## Example code
See examples/OSCAudioMIDIsubsFS.ino for an example of usage. 
