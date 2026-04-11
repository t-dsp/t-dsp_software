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

#include "OSCAudioBase.h"

// class-static variables:
OSCAudioBase* OSCAudioBase::first_route = NULL;
#if defined(DYNAMIC_AUDIO_AVAILABLE) // build create functions
OSCAudioResourceSetting_t OSCAudioBase::settings[rsrc_COUNT] = {0};
#endif // defined(DYNAMIC_AUDIO_AVAILABLE) // build create functions


/* ******************************************************************
 * Generate an array with the names of all the available AudioStream-derived objects,
 * together with pointers to the functions to call to create instances of each.
 */
#if defined(DYNAMIC_AUDIO_AVAILABLE) // build create functions

// objects with (void) constructors:
#define OSC_CLASS(a,o,c) \
OSCAudioBase* mk1_##o(const char* nm) {return (OSCAudioBase*) new o(nm);} \
OSCAudioBase* mk2_##o(const char* nm,OSCAudioGroup& grp) {return (OSCAudioBase*) new o(nm,grp);} 
OSC_AUDIO_CLASSES
#undef OSC_CLASS

// objects with (int) constructors:
#define OSC_CLASSi(a,o,c) \
OSCAudioBase* mk1_##o(const char* nm, int n) {return (OSCAudioBase*) new o(nm,n);} \
OSCAudioBase* mk2_##o(const char* nm,OSCAudioGroup& grp,int n) {return (OSCAudioBase*) new o(nm,grp,n);} 
OSC_AUDIO_CLASSESi
#undef OSC_CLASSi

// objects with (int,float) constructors:
#define OSC_CLASSif(a,o,c) \
OSCAudioBase* mk1_##o(const char* nm, int n, float f) {return (OSCAudioBase*) new o(nm,n,f);} \
OSCAudioBase* mk2_##o(const char* nm,OSCAudioGroup& grp,int n, float f) {return (OSCAudioBase*) new o(nm,grp,n,f);} 
OSC_AUDIO_CLASSESif
#undef OSC_CLASSif

// special cases
const char* AudioMixerName = "AudioMixer";
OSCAudioBase* mk1_AudioMixer(const char* nm, unsigned char ninputs) {return (OSCAudioBase*) new OSCAudioMixer(nm,ninputs);}
OSCAudioBase* mk2_AudioMixer(const char* nm, OSCAudioGroup& grp, unsigned char ninputs) {return (OSCAudioBase*) new OSCAudioMixer(nm,grp,ninputs);} 
#if defined(MULTI_CENTRED) // signals we have a stereo mixer available
const char* AudioMixerStereoName = "AudioMixerStereo";
OSCAudioBase* mk1_AudioMixerStereo(const char* nm, unsigned char ninputs) {return (OSCAudioBase*) new OSCAudioMixerStereo(nm,ninputs);}
OSCAudioBase* mk2_AudioMixerStereo(const char* nm, OSCAudioGroup& grp, unsigned char ninputs) {return (OSCAudioBase*) new OSCAudioMixerStereo(nm,grp,ninputs);} 
#endif // defined(MULTI_CENTRED)

// some classes need resource checks
#define OSC_AUDIO_CHECK_RSRC_noRequirementCheck(...)
#define checkResource_noRequirementCheck NULL
#define OSC_AUDIO_CHECK_RSRC(c) rsrcState_e checkResource_##c(void) {return OSCAudioBase::claimResource(c::resources,COUNT_OF(c::resources),c::rsrcState);}
OSC_AUDIO_CHECK_RSRC_LIST
#undef OSC_AUDIO_CHECK_RSRC

// build table of pointers to creation functions
#define OSC_CLASS(a,o,c)  {#a,.root = {.mk    = mk1_##o},.group = {.mk    = mk2_##o},checkResource_##c,objcVoid},
#define OSC_CLASSi(a,o,c) {#a,.root = {.mk_i  = mk1_##o},.group = {.mk_i  = mk2_##o},checkResource_##c,objcInt},
#define OSC_CLASSif(a,o,c){#a,.root = {.mk_if = mk1_##o},.group = {.mk_if = mk2_##o},checkResource_##c,objcIntFloat},
const OSCAudioTypes_t OSCAudioBase::audioTypes[] = {
  OSC_AUDIO_CLASSES
  OSC_AUDIO_CLASSESi
  OSC_AUDIO_CLASSESif
  //{"jim",mk1_OSCAudioAmplifier,mk2_OSCAudioAmplifier,checkResource_noRequirementCheck,objcVoid},
  //{"bob",.root = {.mk_i = mk1_OSCAudioMixer},.group = {.mk_i = mk2_OSCAudioMixer},checkResource_noRequirementCheck,objcInt}
};
#undef OSC_CLASS
#undef OSC_CLASSi
#undef OSC_CLASSif

#else // no dynamic audio, just a list of class names
	
#define OSC_CLASS(a) {#a},
#define OSC_CLASSi(a) {#a},
#define OSC_CLASSif(a) {#a},
const OSCAudioTypes_t OSCAudioBase::audioTypes[] = {
  OSC_AUDIO_CLASSES
};
#undef OSC_CLASS
#undef OSC_CLASSi
#undef OSC_CLASSif

#endif // defined(DYNAMIC_AUDIO_AVAILABLE)

// Return number of available audio objects
size_t OSCAudioBase::countOfAudioTypes(void) {return COUNT_OF(audioTypes);}
#define CONNECTION_INDEX -1 //!< special "object number" denoting AudioConnection rather than AudioStream object type
//#define AUDIOMIXER_INDEX -2 //!< special variable-width mixer
//#define AUDIOMIXERSTEREO_INDEX -3 //!< special variable-width mixer - stereo
// ********* end of magic array generator stuff ********************


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


//-------------------------------------------------------------------------------------------------------
// Link into a grouping list
void OSCAudioBase::linkInGroup(OSCAudioBase* p) 
{
	OSCAudioGroup* parent = (OSCAudioGroup*) p;
	if (NULL != parent)
	{
		next_route = parent->next_group; 
		parent->next_group = this;
		pParent = parent;
	}
}


// Unlink from a grouping list
void OSCAudioBase::linkOutGroup(OSCAudioGroup** p) 
{
	OSCAudioGroup* parent = *p;
	if (NULL != parent)
	{
		OSCAudioBase** ppLink = &(parent->next_group);
		
		while (NULL != *ppLink && this != *ppLink)
		{
			ppLink = &((*ppLink)->next_route);
			OSC_SPTF("%08X ... ",(uint32_t) *ppLink); OSC_SFSH();
		}
		if (NULL != ppLink)
		{
			OSC_SPTF("Unlink!\n"); OSC_SFSH();
			*ppLink = next_route;
			next_route = NULL;
		}		
		*p = NULL;
	}	
}


//-------------------------------------------------------------------------------------------------------
/*
 * Copy message string parameter into pre-allocated memory buffer.
 * The buffer pointer MUST have enough space for the string, but may be NULL (e.g. if the 
 * allocation failed) in which case the string is not copied.
 * If slashPad is true then we prepend the string with a / if there isn't already one there.
 * \return pointer to buffer, cast to char*
 */
char* OSCAudioBase::getMessageString(OSCMessage& msg, 	//!< message to extract string from
									 int position, 		//!< position in parameter list we expect to find it
									 void* vbuf,		//!< big enough buffer, or NULL
									 bool slashPad)		//!< flag to say string should have / prepended if it's not supplied
{
	char* buf = (char*) vbuf;
	
	if (NULL != buf)
	{
		// length must be OK, no further check needed
		if (slashPad) // see if we want to prepend / if one doesn't exist
			msg.getString(position,buf,1,0,1); // get first character: beware, NO NULL terminator!
		else
			*buf = '/'; // we don't, pretend it's already got one!
		
		if ('/' == * buf)
			msg.getString(position,buf);
		else
		{
			msg.getString(position,buf+1);
			*buf = '/';
		}
		OSC_SPTF("Message string: %s\n",buf);
	}
	
	return buf;
}


//-------------------------------------------------------------------------------------------------------
struct renameObject_s {char* newName; int count; int duplicates; OSCAudioBase* hit;};
void OSCAudioBase::renameObjectCB(OSCAudioBase* ooi,OSCMessage& msg,int offset,void* ctxt) 
{
	renameObject_s* context = (renameObject_s*) ctxt;
	OSCAudioBase* startFrom = ooi->pParent;
	
	if (0 == findMatch(context->newName,NULL,startFrom,false)) // check no clash in this group
	{
		ooi->setName(context->newName);
		context->count++;
		context->hit = ooi;
	}
	else
		context->duplicates++;
}


/**
 *	Rename an [OSC]AudioStream or Connection object.
 *  This could be an /audio function, but that would pollute the 
 *	audio functions' name spaces, so we make it a /dynamic
 *  function instead.
 */
void OSCAudioBase::renameObject(OSCMessage& msg, int addressOffset, OSCBundle& reply)
{
	error retval = OK;
	char* oldName,*newName,*buf;
	renameObject_s context = {0};
	
	oldName = getMessageString(msg,0,alloca(msg.getDataLength(0)+1),true); // could be path, so user is responsible for this being valid
	newName = getMessageString(msg,1,alloca(msg.getDataLength(1)));
	
	if (NULL != oldName && NULL != newName)
	{
		buf = (char*) alloca(strlen(oldName) + strlen(newName) + 8 + 2*15); // [plenty of] space for reply string
		if (NULL != buf)
		{
			OSC_SPTF("new name is %s, size %d\n",newName,strlen(newName));
			trimUnderscores(sanitise(newName,newName),newName); // make the new name valid

			if (0 != strlen(newName)) // zero-length names are not allowed
			{
				context.newName = newName;		
				callBack(oldName,renameObjectCB,&context);
				if (0 == context.count)
					retval = NOT_FOUND; // no object to rename was found
				else if (0 != context.duplicates)
					retval = DUPLICATE_NAME; // object(s) by requested name already exists
			}
			else
				retval = BLANK_NAME; // new name [sanitises to] an empty string			
			
			if (NULL != buf)
			sprintf(buf,"%s -> %s (%d,%d)",oldName,newName,context.count,context.duplicates);
			staticPrepareReplyResult(msg,reply).add(buf).add(retval);
		}
	}
	// alloca() allocates on function stack, so no need to free
}


//-------------------------------------------------------------------------------------------------------
/**
 * Sanitise a string to contain only characters valid in an OSC container or method.
 * Replaces all invalid characters <space>#*,/?[]{} with _.
 * \return pointer to result string (same as dst)
 */
char* OSCAudioBase::sanitise(const char* src, //!< source string
							       char* dst, //!< destination string: may be same as src
								   int offset) //!< offset into sanitisation string: set to 1 to allow /
{
	char* dstCopy = dst;
	const char* toChange = "/ #*,?[]{}";
	
	if (NULL != src && NULL != dst)
	{
		while (0 != *src)
		{
			if (NULL == strchr(toChange+offset,*src))
				*dst++ = *src;
			else
				*dst++ = '_';
			src++;
		}
		*dst = 0;
	}
	
	return dstCopy;
}


/**
 * Trim leading and trailing underscores from a string, and compress internal runs.
 * \return pointer to result string (same as dst)
 */
char* OSCAudioBase::trimUnderscores(const char* src, //!< source string
							         char* dst) //!< destination string: may be same as src
{
	char* dstCopy = dst;
	bool inBody = false,lastWas_ = false;

	if (NULL != src && NULL != dst)
	{
		while (0 != *src)
		{
			if ('_' == *src)
			{
				if (inBody && !lastWas_)
					*dst++ = *src;
				lastWas_ = true;
			}
			else
			{
				*dst++ = *src;
				inBody = true;
				lastWas_ = false;
			}
			src++;
		}
		
		if (lastWas_ && dst != dstCopy) // trailing underscore in non-empty result...
			dst--;	  					// ...overwrite it
		*dst = 0; // terminate result
	}
	
	return dstCopy;
}


//-------------------------------------------------------------------------------------------------------


//=======================================================================================================
//============================== Dynamic Audio Objects ==================================================
static void hitCountCB(OSCAudioBase* ooi,OSCMessage& msg,int offset,void* count) {(*(int*) count)++;}
/**
 *	Count the number of objects that match the supplied address.
 */
int OSCAudioBase::hitCount(const char* addr,	//!< address to match
							OSCAudioBase* ooi,	//!< where in structure to start from (default is root)
							bool enterGroups)	//!< whether to allow matches in sub-groups
{
	int result = 0;
	
	callBack(addr,hitCountCB,(void*) &result,ooi,enterGroups);
	
	return result;
}


struct findMatch_s {int count; OSCAudioBase* hit;};
static void findMatchCB(OSCAudioBase* ooi,OSCMessage& msg,int offset,void* ctxt) 
{
	findMatch_s* context = (findMatch_s*) ctxt;
	context->count++;
	context->hit = ooi;
}
/**
 *	Count the number of objects that match the supplied address, and return a pointer to the last found
 */
int OSCAudioBase::findMatch(const char* addr,		//!< address to match
							OSCAudioBase** found,	//!< last-found matching object
							OSCAudioBase* ooi,		//!< where in structure to start from (default is root)
							bool enterGroups)		//!< whether to allow matches in sub-groups
{
	findMatch_s result = {0};
	
	callBack(addr,findMatchCB,(void*) &result,ooi,enterGroups);
	if (NULL != found)	// allow for not caring about the last-hit object
		*found = result.hit; // will be NULL if nothing found
	
	return result.count;
}
		 
		 

/**
 *	Route message for the creation engine to the correct function.
 */
void OSCAudioBase::routeDynamic(OSCMessage& msg, int addressOffset, OSCBundle& reply)
{
    if (isStaticTarget(msg,addressOffset,"/renameObject","ss")) {renameObject(msg,addressOffset,reply);} 
#if defined(DYNAMIC_AUDIO_AVAILABLE) // route OSC commands to create / destroy objects
    else if (isStaticTarget(msg,addressOffset,"/crCo","s"))  {createConnection(msg,addressOffset,reply);} 
    else if (isStaticTarget(msg,addressOffset,"/crCo","ss")) {createConnection(msg,addressOffset,reply);} 
    else if (isStaticTarget(msg,addressOffset,"/crOb","ss*"))	 {createObject(msg,addressOffset,reply);} 
/*	
    else if (isStaticTarget(msg,addressOffset,"/crOb","sss"))	 {createObject(msg,addressOffset,reply);} 
    else if (isStaticTarget(msg,addressOffset,"/crOb","ssi"))	 {createObject(msg,addressOffset,reply);} 
    else if (isStaticTarget(msg,addressOffset,"/crOb","sssi"))	 {createObject(msg,addressOffset,reply);} 
	*/
    else if (isStaticTarget(msg,addressOffset,"/crGr","ss"))	 {createGroup(msg,addressOffset,reply);} 
    else if (isStaticTarget(msg,addressOffset,"/crGrp","ss"))	 {createGroup(msg,addressOffset,reply);} 
    else if (isStaticTarget(msg,addressOffset,"/deOb","s"))	 {destroyObject(msg,addressOffset,reply);} 
    //else if (isStaticTarget(msg,addressOffset,"/clearAll",NULL)) 		 {clearAllObjects(msg,addressOffset,reply);} 
    else if (isStaticTarget(msg,addressOffset,"/createConnection","s"))  {createConnection(msg,addressOffset,reply);} 
    else if (isStaticTarget(msg,addressOffset,"/createConnection","ss")) {createConnection(msg,addressOffset,reply);} 
    else if (isStaticTarget(msg,addressOffset,"/createObject","ss*"))	 {createObject(msg,addressOffset,reply);}
/*	
    else if (isStaticTarget(msg,addressOffset,"/createObject","sss"))	 {createObject(msg,addressOffset,reply);} 
    else if (isStaticTarget(msg,addressOffset,"/createObject","ssi"))	 {createObject(msg,addressOffset,reply);} 
    else if (isStaticTarget(msg,addressOffset,"/createObject","sssi"))	 {createObject(msg,addressOffset,reply);} 
*/
    else if (isStaticTarget(msg,addressOffset,"/createGroup","ss"))	 {createGroup(msg,addressOffset,reply);} 
    else if (isStaticTarget(msg,addressOffset,"/destroyObject","s"))	 {destroyObject(msg,addressOffset,reply);} 
    else if (isStaticTarget(msg,addressOffset,"/clearAll",NULL)) 		 {clearAllObjects(msg,addressOffset,reply);} 
	else
	{		
		size_t addrL = getMessageAddressLen(msg) - addressOffset;
		char* buf= getMessageAddress(msg,alloca(addrL),addrL,addressOffset);
		OSC_SPTF("No match\n");
		staticPrepareReplyResult(msg,reply).add(buf).add((int) NOT_ROUTED);
	}
#endif // defined(DYNAMIC_AUDIO_AVAILABLE)
}


#if defined(DYNAMIC_AUDIO_AVAILABLE) // actual creation / destruction functions
/**
 *	Destroy an [OSC]AudioStream or Connection object.
 */
void OSCAudioBase::destroyObject(OSCMessage& msg, int addressOffset, OSCBundle& reply)
{
	char* vName;
	OSCAudioBase* pVictim;
	int matchCount = 0;
	
	vName = getMessageString(msg,0,alloca(msg.getDataLength(0)+1),true); // may be a path with wildcards, user has to ensure it's valid
	
	if (NULL != vName)
	{
		OSC_SPRT("destroyObject: ");
		OSC_SPLN(vName);
		OSC_DBGP(msg,addressOffset); 
		
		// Have to do repeated searches rather than one callBack() sweep,
		// because removing items from a branched list while you're
		// traversing it is bound to result in Alarm and Despondency
		do
		{
			int numFound = OSCAudioBase::findMatch(vName,&pVictim);
			if (numFound > 0)
			{
				matchCount++;
				OSC_SPTF("Victim is at: 0x%08X\n",(uint32_t) pVictim);
				OSC_SFSH();
				if (NULL != pVictim) // shouldn't need to check, but...
					delete pVictim;
			}
		} while (NULL != pVictim);
		
		if (0 == matchCount)
		{
			OSC_SPLN("not found!"); // but not really an error!
		}
		
		staticPrepareReplyResult(msg,reply).add(vName).add(matchCount).add((int) OK);
	}
}

/**
 *	Destroy all dynamic OSCAudioStream and OSCAudioConnection objects.
 *  Note that this will not destroy objects unknown to the OSCAudio system.
 */
void OSCAudioBase::clearAllObjects(OSCMessage& msg, int addressOffset, OSCBundle& reply)
{
	OSC_SPRT("clearAllObjects: ");
	OSC_DBGP(msg,addressOffset);
	
	while (NULL != first_route)
	{
		OSC_SPLN(first_route->name);
		OSC_SFSH();
		delete first_route;
	}
	staticPrepareReplyResult(msg,reply).add("ALL").add((int) OK);
}

//============================== OSCAudioStream =========================================================
struct crObContext_s {int objIdx; const char* name; OSCAudioBase::error retval; objcType_e cType; int i1; float f1;};
//-------------------------------------------------------------------------------------------------------
// Test code for switching between full dynamic audio support and simple control
// messaging. If full support is enabled then the object code is MUCH larger, but
// it is possible to create objects at runtime.
#define noDISABLE_FULL_DYNAMIC
#if defined(DISABLE_FULL_DYNAMIC)
// weak definition
OSCAudioBase::error DynamicAudioCreateObject(char* typ,char* objName,OSCAudioBase* p,void* x) __attribute__((weak));	
											 
OSCAudioBase::error DynamicAudioCreateObject(char* typ,char* objName,OSCAudioBase* p,void* x)
{
	return OSCAudioBase::NO_DYNAMIC;
}

#else
// strong definition: use this if object creation at runtime is wanted
/**
 *	Create a new [OSC]AudioStream object.
 */
OSCAudioBase::error DynamicAudioCreateObject(int objIdx,			//!< index of [OSC]AudioStream object to create
											 const char* objName,	//!< name of object
											 OSCAudioBase* parent, 	//!< parent object
											 void* ctxt = NULL)		//!< additional parameters, maybe
{
	OSCAudioBase::error retval = OSCAudioBase::OK;
	void* pNewObj = NULL;
	OSCAudioBase* png = parent;
	int hits = -1;
	crObContext_s* context = (crObContext_s*) ctxt;
	
	if (NULL != png) // has a parent group
	{
		png = parent->getNextGroup();
		if (NULL != png) // group has no members, can't have hits!
			hits = OSCAudioBase::hitCount(objName,png,false); // count matches
	}
	else
		hits = OSCAudioBase::hitCount(objName,png,false); // count from root
		
	OSC_SPTF("Found %d duplicates of %s from %08X\n",hits,objName,png);
	
	if (hits > 0)
		retval = OSCAudioBase::DUPLICATE_NAME;
	else
	{
		rsrcState_e rsrcState = rsrcFree;
		if (objIdx >= 0
		 && NULL != OSCAudioBase::audioTypes[objIdx].chkResource) // if it needs claiming...
			rsrcState = OSCAudioBase::audioTypes[objIdx].chkResource(); // ...try to claim
			
		if (rsrcState >= rsrcThisActive) // can't create, clashes
			retval = OSCAudioBase::IN_USE;
		else // OK, create it!
		{
			if (NULL == parent) // create at root
			{
				switch (objIdx)
				{
				  default:
					switch (OSCAudioBase::audioTypes[objIdx].constructorType)
					{
						case objcVoid:
							pNewObj = OSCAudioBase::audioTypes[objIdx].root.mk(objName+1);
							break;
							
						case objcInt:
							if (NULL != context && objcInt == context->cType)
								pNewObj = OSCAudioBase::audioTypes[objIdx].root.mk_i(objName+1,context->i1);
							break;
							
						case objcIntFloat:
							if (NULL != context && objcIntFloat == context->cType)
								pNewObj = OSCAudioBase::audioTypes[objIdx].root.mk_if(objName+1,context->i1,context->f1);
							break;
					}
					break;
						
				  case CONNECTION_INDEX:
					pNewObj = new OSCAudioConnection(objName+1);
					break;
				}
			}
			else // create in a group
			{
				switch (objIdx)
				{
				  default:
					switch (OSCAudioBase::audioTypes[objIdx].constructorType)
					{
						case objcVoid:
							pNewObj = OSCAudioBase::audioTypes[objIdx].group.mk(objName+1,*((OSCAudioGroup*) parent));
							break;
							
						case objcInt:
							if (NULL != context && objcInt == context->cType)
								pNewObj = OSCAudioBase::audioTypes[objIdx].group.mk_i(objName+1,*((OSCAudioGroup*) parent),context->i1);
							break;
							
						case objcIntFloat:
							if (NULL != context && objcIntFloat == context->cType)
								pNewObj = OSCAudioBase::audioTypes[objIdx].group.mk_if(objName+1,*((OSCAudioGroup*) parent),context->i1,context->f1);
							break;
					}
					break;
										
				  case CONNECTION_INDEX:
					pNewObj = new OSCAudioConnection(objName+1,*((OSCAudioGroup*) parent));
					break;
				}
			}
		}
		
		if (NULL != pNewObj)
		{
			const char* name;
			switch (objIdx)
			{
			  default:
				name = OSCAudioBase::audioTypes[objIdx].name;
				break;
				
			  case CONNECTION_INDEX:
				name = "connection";
				break;
			}
			OSC_SPTF("Created %s as a new %s at %08X\n",objName, name, (uint32_t) pNewObj); (void) name;
		}
	}
	return retval;
}
#endif // defined(DISABLE_FULL_DYNAMIC)
//-------------------------------------------------------------------------------------------------------


/**
 * Check to see if a resource is available for use.
 * It will not be if it's not possible to share it, or if the required settings differ from
 * those currently in place.
 */
rsrcState_e OSCAudioBase::checkResource(const OSCAudioResourceCheck_t* reqRsrc,  	//!< resources needed
													  int nRsrc,			//!< count of resources
													  rsrcState_e curState)	//!< current state
{
	rsrcState_e result = rsrcFree;
	
	for (int i=0;i<nRsrc && result < rsrcThisActive;i++)
	{
		resourceSetting_e currentSetting = settings[reqRsrc[i].resource].setting; // get current setting
		
		switch (currentSetting)
		{
			case setgAvailable: // not in use, OK
				break;
				
			case setgUnshareable: // in use, not shareable
				if (settings[reqRsrc[i].resource].resArray != reqRsrc) // not us using it, either
					result = rsrcOther; // can't use
				else
					result = curState;  // OK if our first use or we're dormant; not OK if we're active
			
			default: // some other setting
				if (settings[reqRsrc[i].resource].resArray != reqRsrc) // not us using it
				{
					// can't share the resource if the existing owner needs different settings, 
					// UNLESS either the existing owner has flexible settings and the new sharer can use one of those,
					// OR the existing owner has a fixed setting but the new sharer is flexible and can use the current setting
					if (setg_Teensy_ControlOrSPDIF_Control == currentSetting  // current owner is flexible...
					 && (setg_Teensy_Control == reqRsrc[i].setting			  // ...new sharer can use one...
					  || setg_SPDIF_Control  == reqRsrc[i].setting)			  // ...of the settings
					  )
						break;  // OK, we have a result
					 
					if (setg_Teensy_ControlOrSPDIF_Control == reqRsrc[i].setting   // new sharer is flexible...
					 && (setg_Teensy_Control == currentSetting			  // ...current owner can use one...
					  || setg_SPDIF_Control  == currentSetting)			  // ...of the settings
					  )
						break;  // OK, we have a result
					 
					if (currentSetting != reqRsrc[i].setting) // are current and required settings different?
						result = rsrcOther; // yup, can't use it
				}
				break;
		}		
	}
	
	return result;
}


/**
 * Claim a resource if it's available for use.
 * It will not be if it's not possible to share it, or if the required settings differ from
 * those currently in place.
 */
rsrcState_e OSCAudioBase::claimResource(const OSCAudioResourceCheck_t* reqRsrc,  	//!< resources needed
													  int nRsrc,			//!< count of resources
													  rsrcState_e curState)	//!< current state
{
	rsrcState_e result = checkResource(reqRsrc,nRsrc,curState);
	if (result < rsrcThisActive) // if we can claim it
	{
		for (int i=0;i<nRsrc;i++)
		{
			// if the resource is in use and the new sharer has more flexibility, leave
			// the resource setting in the more stringent state
			if (setgAvailable == settings[reqRsrc[i].resource].setting 
			 || setg_Teensy_ControlOrSPDIF_Control != reqRsrc[i].setting)
			{
				settings[reqRsrc[i].resource].setting = reqRsrc[i].setting; // set current setting
				settings[reqRsrc[i].resource].resArray = reqRsrc; //and which object is using it
			}
		}
	}
	return result;
}



void createObjectCB(OSCAudioBase* parent,OSCMessage& msg,int offset,void* ctxt)
{
	crObContext_s* context = (crObContext_s*) ctxt;

	OSCAudioBase::error rv = DynamicAudioCreateObject(context->objIdx,context->name,parent,ctxt);
	
	if (rv != OSCAudioBase::OK)
		context->retval = rv;
}


/**
 *	Create a new [OSC]AudioStream object.
 * 	OSC parameters are at least 'ss', for type and name. If the next is 's' then it's the group name.
 *	Subsequent parameters are currently allowed to be 'i' or 'if'.
 */
void OSCAudioBase::createObject(OSCMessage& msg, int addressOffset, OSCBundle& reply)
{
	error retval = OK;
	char* objName,*typ;
	int objIdx;
	int ml = msg.size() - 1; // max message parameter index: at least 1, as we must have type and name
	bool isRoot = !(ml>1 && msg.isString(2)); // sss or sssi, but not ssi, are group creates
	int extra = isRoot?2:3; // this is where extra data may be found
	
	typ = getMessageString(msg,0,alloca(msg.getDataLength(0)));
	objName = getMessageString(msg,1,alloca(msg.getDataLength(1)+1),true);
	
	crObContext_s context = {-999,objName,retval,objcVoid};
	
	// get any optional parameters
	if (extra <= ml && msg.isInt(extra))
	{
		context.i1 = msg.getInt(extra);
		context.cType = objcInt;
		extra++;
		if (extra <= ml && msg.isFloat(extra))
		{
			context.f1 = msg.getFloat(extra);
			context.cType = objcIntFloat;
		}
	}
						
	if (NULL != typ && NULL != objName)
	{
		objIdx = OSCAudioBase::getTypeIndex(typ); // see if we've got a valid object type
		if (objIdx >= 0 && context.cType == audioTypes[objIdx].constructorType) // valid type and parameters
		{	
			if (strlen(objName+1) > 0) // initial / doesn't count as part of the name!
			{
				trimUnderscores(sanitise(objName,objName,1),objName); // make the name valid
				context.objIdx = objIdx;
				
				OSC_SPTF("createObject(%s,%s)\n",typ,objName);
				OSC_DBGP(msg,addressOffset);
				
				if (isRoot) // create in root, not group
					retval = DynamicAudioCreateObject(objIdx,objName,NULL,&context);
				else
				{
					char* path = getMessageString(msg,2,alloca(msg.getDataLength(2)+1),true);
					
					if (NULL != path)
					{
						callBack(path,createObjectCB,&context);
						retval = context.retval;
					}
				}
			}
			else
				retval = BLANK_NAME;
		}
		else 
			retval = NOT_FOUND;
			
		staticPrepareReplyResult(msg,reply).add(objName).add((int) retval);
	}
}

#endif // defined(DYNAMIC_AUDIO_AVAILABLE)

//============================== OSCAudioGroup =====================================================
OSCAudioGroup::OSCAudioGroup(const char* _name, OSCAudioGroup* parent) 
							:  OSCAudioBase(_name), first_src(NULL), first_dst(NULL) 
{
	if (NULL != parent) // we're a group member not on the main routing list
	{
		linkOut();
		linkInGroup(parent);
		OSC_SPTF("%s group re-linked:\n",name);
	}
}

	
OSCAudioGroup::~OSCAudioGroup()
{
	OSC_SPTF("dtor for %s\n",name);
	while (NULL != next_group)
		delete next_group;			
	while (NULL != first_src)
		delete first_src;			
	while (NULL != first_dst)
		delete first_dst;	
	linkOutGroup();
}

/**
 * Return pointer to next OSC audio group after given one
 */
OSCAudioBase* OSCAudioGroup::getNextGroup(void)
{
	return next_group;
}
	
	
/**
 * Route message to members of this group, if the group name matches.
 */
void OSCAudioGroup::route(OSCMessage& msg, int addressOffset, OSCBundle& reply)
{
	int nameOff = isMine(msg,addressOffset);
	if (nameOff > 0)
	{ 
		routeFrom((OSCAudioBase**) &next_group,msg,addressOffset+nameOff,reply);
	}
}

// Link in and out of the grouping lists
void OSCAudioGroup::linkInGroup(OSCAudioGroup* parent) 
{
	if (NULL != parent)
	{
		next_route = parent->next_group; 
		parent->next_group = this;
		pParent = parent;
	}
}

void OSCAudioGroup::linkOutGroup() 
{
	if (NULL == pParent) // in main routing list...
		linkOut();				 // ...unlink from that
	else
	{
		OSCAudioBase** ppLink = &(pParent->next_group); 
		
		while (NULL != *ppLink && this != *ppLink)
			ppLink = &((*ppLink)->next_group);
		if (NULL != ppLink)
		{
			*ppLink = next_group;
			next_group = NULL;
		}
	}
}




#if defined(DYNAMIC_AUDIO_AVAILABLE) // Group creation / destruction
struct crGrpContext_s {const char* name; OSCAudioBase::error retval;};
void createGroupCB(OSCAudioBase* parent,OSCMessage& msg,int offset,void* context)
{
	crGrpContext_s* ctxt = (crGrpContext_s*) context;
	const char* buf = ctxt->name;
	OSCAudioBase* png = parent;
	
	if (NULL != png)
		png = parent->getNextGroup();
	
	if (0 != OSCAudioBase::hitCount(buf,png,false))
	{
		ctxt->retval = OSCAudioBase::DUPLICATE_NAME;
	}
	else
	{
		OSCAudioGroup* pNewGroup = new OSCAudioGroup(buf+1,(OSCAudioGroup*) parent);
		(void) pNewGroup;
		OSC_SPTF("Created %s at: 0x%08X, parent %08X\n",buf,(uint32_t) pNewGroup,(uint32_t) (pNewGroup->pParent));
	}	
}


/**
 *	Create a new OSCAudioGroup object.
 */
void OSCAudioBase::createGroup(OSCMessage& msg, int addressOffset, OSCBundle& reply)
{
	error retval = OK;
	char* buf, *path;
	
	OSC_SPLN("createGroup");
	OSC_DBGP(msg,addressOffset);
	buf  = getMessageString(msg,0,alloca(msg.getDataLength(0)+1),true);
	path = getMessageString(msg,1,alloca(msg.getDataLength(1)+1),true);
	trimUnderscores(sanitise(buf,buf,1),buf); // make the name valid
	OSC_SPTF("%s%s\n",path,buf);
	
	if (NULL != buf && NULL != path)
	{
		crGrpContext_s context = {buf,OSCAudioBase::OK};
		if (1 == strlen(path)) // just a / : create directly at root
			createGroupCB(NULL,msg,0,&context);
		else	// sub-group of existing group
			callBack(path,createGroupCB,&context);
		retval = context.retval;
	}
	else
		retval = BLANK_NAME;
	
	staticPrepareReplyResult(msg,reply).add(buf).add((int) retval);
}
#endif // defined(DYNAMIC_AUDIO_AVAILABLE)

//============================== OSCAudioConnection =====================================================
/**
 * Destroy an OSCAudioConnection: ensure it's unlinked from its lists.
 */
OSCAudioConnection::~OSCAudioConnection(void)
{
	OSC_SPTF("dtor for %s\n",name);
	linkOutSrc();
	linkOutDst();
}

#if defined(DYNAMIC_AUDIO_AVAILABLE) // Connection creation / destruction
/**
 *	Create a new [OSC]AudioConnection object.
 */
void OSCAudioBase::createConnection(OSCMessage& msg, int addressOffset, OSCBundle& reply)
{
	char* objName;
	error retval = OK;
	
	OSC_SPLN("createConnection");
	OSC_DBGP(msg,addressOffset);
	objName = getMessageString(msg,0,alloca(msg.getDataLength(0)+1),true);
	
	if (NULL != objName)
	{
		OSC_SPLN(objName);
		
		if (strlen(objName) > 0)
		{
			trimUnderscores(sanitise(objName,objName,1),objName); // make the name valid
			if (msg.size() > 1) // then we want to put the connection in one or more groups
			{
				crObContext_s context;
				char* path = getMessageString(msg,1,alloca(msg.getDataLength(1)+1),true);
				
				context = {CONNECTION_INDEX,objName,OK};	// use special "index" to create connections
				callBack(path,createObjectCB,&context); 	// NULL path is safe, no check needed
				retval = context.retval;			
			}
			else 
				retval = DynamicAudioCreateObject(CONNECTION_INDEX,objName,NULL);	
		}
		else
			retval = BLANK_NAME;
		
		staticPrepareReplyResult(msg,reply).add(objName).add((int) retval);
	}
}


/**
 *	Connect an [OSC]AudioConnection object to specific object ports
 */
void OSCAudioConnection::OSCconnect(OSCMessage& msg, 
								 int addressOffset,
								 OSCBundle& reply, 
								 bool zeroToZero)  	//!< true to use port 0 on both, otherwise they're in the message
{
	char* srcn,*dstn,*buf;
	AudioStream* src = NULL,*dst = NULL;
	int srcp=0,dstp=0,count;
	OSCAudioBase* srcB,*dstB;
	error retval = OK;
	
	OSC_SPLN("makeConnection");
	OSC_DBGP(msg,addressOffset);
	
	srcn = getMessageString(msg,0,alloca(msg.getDataLength(0)+1),true);
	trimUnderscores(sanitise(srcn,srcn,1),srcn); // make the source name valid
	if (!zeroToZero)
	{
		srcp = msg.getInt(1);
		dstn = getMessageString(msg,2,alloca(msg.getDataLength(2)+1),true);
		dstp = msg.getInt(3);
	}
	else
	{
		dstn = getMessageString(msg,1,alloca(msg.getDataLength(1)+1),true);
	}		
	trimUnderscores(sanitise(dstn,dstn,1),dstn); // make the destination name valid
	buf = alloca(strlen(srcn) + strlen(dstn) + 9 +15*3);
	
	if (NULL != srcn && NULL != dstn && NULL != buf)
	{
		OSC_SPTF("%s, %s\n",srcn,dstn);
		
		// Find the named OSCAudioBase objects and convert to
		// the corresponding AudioStream: is there a better way?
		if (1 == (count = findMatch(srcn,&srcB))) // just one source, please
		{
			src = srcB->sibling;
			if (1 == (count = findMatch(dstn,&dstB))) // and one destination!
				dst = dstB->sibling;			
			else
				retval = (0 == count)?NOT_FOUND:AMBIGUOUS_PATH;
		}
		else
			retval = (0 == count)?NOT_FOUND:AMBIGUOUS_PATH;
		
		if (OK == retval)
		{
			if (NULL != src && NULL != dst)
			{
				int connResult = connect(*src,(int) srcp,*dst,(int) dstp); // make the audio connection
				sprintf(buf,"%s:%d -> %s:%d (%d)",srcn,srcp,dstn,dstp,connResult);
				switch (connResult) // this may be unreliable, result numbers are not really supported
				{
					case 0: // OK
						break;
					
					case 4: // destination in use
					case 6: // connection already in place
						retval = IN_USE;
						break;
						
					default: // object or input number not found
						retval = NOT_FOUND;
						break;
				}
				mkLinks(*srcB,*dstB); // make the OSC linkages
			}
			else
			{
				sprintf(buf,"Nothing!");
				retval = NOT_FOUND;
			}
		}
		else
		{
			sprintf(buf,"Bad path(s): %s, %s",srcn,dstn);
		}
		OSC_SPLN(buf);
		
		prepareReplyResult(msg,reply,name).set(1,buf).add((int) retval);
	}
}
#endif // defined(DYNAMIC_AUDIO_AVAILABLE)

/**
 * Compute the space required for a connection name.
 */
int OSCAudioConnection::getLength(const char* name, OSCAudioBase& src,OSCAudioBase& dst, AutoName an)
{
	int result = -1; // assume we're just going to use the name as given
	
	switch (an)
	{
		default:  // get rid of warning
			break;

		case AutoName::Underscores:
			result  = 3; // 
			// fallthrough
		case AutoName::Short:
			result += 6 // worst case for the port numbers
				   + strlen(name)
				   + strlen(src.name) // adds two extra for...
				   + strlen(dst.name) // ...the initial / characters
				   ;
			break;
	}
	
	return result;
}

/**
 * Set a connection name based on its elements.
 */
void OSCAudioConnection::autoSetName(const char* name, 
									 OSCAudioBase& src, uint8_t srcO,
									 OSCAudioBase& dst, uint8_t dstI,
									 AutoName an)
{
	char* buf;
	int bufLen = getLength(name,src,dst,an);
	
	if (bufLen > 0) // we actually do want to change it!
	{
		buf = (char*) alloca(bufLen);
		if (NULL != buf)
		{
			switch (an)
			{
				default:  // get rid of warning
					break;
					
				case AutoName::Underscores:
					sprintf(buf,"%s_%s_%d_%s_%d",name,src.name+1,srcO,dst.name+1,dstI);
					break;
					
				case AutoName::Short:
					sprintf(buf,"%s%s%d%s%d",name,src.name+1,srcO,dst.name+1,dstI);				
					break;
			}
			setName(buf);
		}
	}
}


void OSCAudioConnection::mkLinks(OSCAudioBase& src, OSCAudioBase& dst)
{
	OSC_SPTF("Link %08X to source %08X (parent %08X)\n",
					(uint32_t) this,
					(uint32_t) &src,
					(uint32_t) src.pParent
					);
					
	linkInSrc(src.pParent);
	OSC_SPTF(" and dest %08X (parent %08X)\n\n",
					(uint32_t) &dst,
					(uint32_t) dst.pParent
					);
					
	linkInDst(dst.pParent);
	
	/*
	// should only do this for debug purposes
	AudioConnection* cptr = this;	// find pointer to base AudioConnection
	sibling = (AudioStream*) cptr;	// store it for structure crawler to use
	*/
}


void OSCAudioConnection::route(OSCMessage& msg, int addressOffset, OSCBundle& reply)
{
	int nameOff = isMine(msg,addressOffset);
	if (nameOff > 0)
	{ 
		addressOffset += nameOff;
#if defined(DYNAMIC_AUDIO_AVAILABLE) // route OSC commands to Connection
		if (isTarget(msg,addressOffset,"/c*","ss")) {OSCconnect(msg,addressOffset,reply,true);}
		else if (isTarget(msg,addressOffset,"/c*","sisi")) {OSCconnect(msg,addressOffset,reply);} 
		else if (isTarget(msg,addressOffset,"/d*",NULL)) {int r = disconnect(); addReplyResult(msg,addressOffset,reply,r==0,name,r?NOT_CONNECTED:OK);} 
#endif // defined(DYNAMIC_AUDIO_AVAILABLE)
	}
}


// Link in and out of the connection lists
void OSCAudioConnection::linkInSrc(OSCAudioGroup* parent) 
{
	if (NULL != parent)
	{
		next_src = parent->first_src; 
		parent->first_src = this;
		pSrcParent = parent;
	}
}


void OSCAudioConnection::linkOutSrc() 
{
	if (NULL != pSrcParent)
	{
		OSC_SPTF("Unlink %08X from parent %08X\n",(uint32_t) this,(uint32_t) pSrcParent);
		OSCAudioConnection** ppLink = &(pSrcParent->first_src); 			
		while (NULL != *ppLink && this != *ppLink)
			ppLink = &((*ppLink)->next_src);
		if (NULL != ppLink)
		{
			*ppLink = next_src;
			next_src = NULL;
		}
	}
}

// Link in and out of the connection lists
void OSCAudioConnection::linkInDst(OSCAudioGroup* parent) 
{
	if (NULL != parent)
	{
		next_dst = parent->first_dst; 
		parent->first_dst = this;
		pDstParent = parent;
	}
}

void OSCAudioConnection::linkOutDst() 
{
	if (NULL != pDstParent)
	{
		OSCAudioConnection** ppLink = &(pDstParent->first_dst); 			
		while (NULL != *ppLink && this != *ppLink)
			ppLink = &((*ppLink)->next_dst);
		if (NULL != ppLink)
		{
			*ppLink = next_dst;
			next_dst = NULL;
		}
	}
}

