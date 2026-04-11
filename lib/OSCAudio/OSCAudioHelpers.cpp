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

#define OSC_RSRC_ENABLE_DEFINE_ARRAYS
#include "OSCAudioHelpers.h"

static void* getSpace(void** pptr,size_t len, size_t oldLen)
{	
	void* toFree = *pptr;
	void* result = toFree;
			
	if (len < oldLen || NULL == toFree) // need more space...
	{
		result = malloc(len); // ...get it
	}
	else
	{
		toFree = NULL; // got enough, re-use it
	}

	if (NULL != toFree)
	{
		free(toFree);
	}		
	
	*pptr = result;
	
	return result;
 }

// === FFT window names ===
typedef struct {
    const char* name;
    const int16_t* windowData;
    } FFTwindows_t;


#if defined(analyze_fft256_h_)

const FFTwindows_t FFT256windows[] = {
	"Hanning",AudioWindowHanning256,
	"Bartlett",AudioWindowBartlett256,
	"Blackman",AudioWindowBlackman256,
	"Flattop",AudioWindowFlattop256,
	"BlackmanHarris",AudioWindowBlackmanHarris256,
	"Nuttall",AudioWindowNuttall256,
	"BlackmanNuttall",AudioWindowBlackmanNuttall256,
	"Welch",AudioWindowWelch256,
	"Hamming",AudioWindowHamming256,
	"Cosine",AudioWindowCosine256,
	"Tukey",AudioWindowTukey256,
};

// Set FFT window function by name, e.g. "Hanning"
bool OSCAudioAnalyzeFFT256::windowFunction(OSCMessage& msg)
{
	bool result = false;
	
	char buf[50];
	msg.getString(0,buf,50);
	for (size_t i=0;i<COUNT_OF(FFT256windows);i++)
		if (0 == strcmp(buf,FFT256windows[i].name))
		{
			AudioAnalyzeFFT256::windowFunction(FFT256windows[i].windowData);
			result = true;
			break;
		}
	return result;
}
#endif // defined(analyze_fft256_h_)



#if defined(analyze_fft1024_h_)

const FFTwindows_t FFT1024windows[] = {
	"Hanning",AudioWindowHanning1024,
	"Bartlett",AudioWindowBartlett1024,
	"Blackman",AudioWindowBlackman1024,
	"Flattop",AudioWindowFlattop1024,
	"BlackmanHarris",AudioWindowBlackmanHarris1024,
	"Nuttall",AudioWindowNuttall1024,
	"BlackmanNuttall",AudioWindowBlackmanNuttall1024,
	"Welch",AudioWindowWelch1024,
	"Hamming",AudioWindowHamming1024,
	"Cosine",AudioWindowCosine1024,
	"Tukey",AudioWindowTukey1024,
};

// Set FFT window function by name, e.g. "Hanning"
bool OSCAudioAnalyzeFFT1024::windowFunction(OSCMessage& msg)
{
	bool result = false;	
	char buf[50];
	
	msg.getString(0,buf,50);
	
	for (size_t i=0;i<COUNT_OF(FFT1024windows);i++)
		if (0 == strcmp(buf,FFT1024windows[i].name))
		{
			AudioAnalyzeFFT1024::windowFunction(FFT1024windows[i].windowData);
			result = true;
			break;
		}
	return result;
}
#endif // defined(analyze_fft1024_h_)


#if defined(analyze_print_h_)
// Name analyse print channel. Creates heap storage
// for name string, which is freed on destruction.
bool OSCAudioAnalyzePrint::name(OSCMessage& msg)
{
	bool result = false;	
	char buf[50];
	
	msg.getString(0,buf,50);
	
	namePtr = (char*) getSpace((void**) &namePtr,strlen(buf)+1,0); // get some storage
	if (NULL != namePtr)
	{
		strcpy(namePtr,buf);
		AudioAnalyzePrint::name(namePtr);
		result = true;
	}
	
	return result;
}
#endif // defined(analyze_print_h_)


#if defined(control_sgtl5000_h_)
// Set filter parameters from a 7-element blob
void OSCAudioControlSGTL5000::eqFilter(OSCMessage& msg)
{
	uint8_t filterNum = msg.getInt(0);
	int filterParameters[7];
	
	msg.getBlob(1,(uint8_t*) filterParameters, sizeof filterParameters);
	AudioControlSGTL5000::eqFilter(filterNum,filterParameters);
}
#endif // defined(control_sgtl5000_h_)


#if defined(control_tlv320aic3206_h_)
// Set filter parameters from a 3-coefficient blob
void OSCAudioControlTLV320AIC3206::setIIRCoeffOnADC(OSCMessage& msg)
{
	int chan = msg.getInt(0);
	uint32_t coeff[3];
	
	msg.getBlob(1,(uint8_t*) coeff, sizeof coeff);
	AudioControlTLV320AIC3206::setIIRCoeffOnADC(chan,coeff);
}
#endif // defined(control_tlv320aic3206_h_)



#if defined(_effect_granular_h_)
// Set sample bank from blob
void OSCAudioEffectGranular::begin(OSCMessage& msg)
{
	int16_t max_len_def = msg.getBlobLength(0); // actual blob length
	
	sample_bank = (int16_t*) getSpace((void**) &sample_bank,max_len_def,0); // get some storage
	if (NULL != sample_bank)
	{
		msg.getBlob(0,(uint8_t*) sample_bank,max_len_def);
		max_len_def = msg.getInt(1); // now, how long the user said it was
		AudioEffectGranular::begin(sample_bank,max_len_def);
	}
}
#endif // defined(_effect_granular_h_)


#if defined(filter_biquad_h_)
// Rather than an array / blob, message has 5 filter 
// parameters, either double or int. 
void OSCAudioFilterBiquad::setCoefficients(OSCMessage& msg)
{
	uint32_t stage = msg.getInt(0);
	double coeffsD[5];
	int coeffsI[5];
	
	if (msg.isInt(1)) // integer coeffs
	{
		for (size_t i=1;i<COUNT_OF(coeffsI);i++)
			coeffsI[i] = msg.getInt(i);
		AudioFilterBiquad::setCoefficients(stage,coeffsI);
	}
	else // double coeffs
	{
		for (size_t i=1;i<COUNT_OF(coeffsD);i++)
			coeffsD[i] = msg.getDouble(i);
		AudioFilterBiquad::setCoefficients(stage,coeffsD);
	}
}
#endif // defined(filter_biquad_h_)


#if defined(play_memory_h_)
// DANGER Will Robinson! We'll accept an integer pointer, but you
// better be sure it's valid. Really NOT a good candidate for 
// passing in a blob, here...
void OSCAudioPlayMemory::play(OSCMessage& msg)
{
	unsigned int* data = (unsigned int*) msg.getInt(0);
	AudioPlayMemory::play(data);
}
#endif // defined(play_memory_h_)


#if defined(play_sd_raw_h_)
// Play from named file
void OSCAudioPlaySdRaw::play(OSCMessage& msg)
{
	char buf[50];	
	msg.getString(0,buf,50);
	AudioPlaySdRaw::play(buf);
}
#endif // defined(play_sd_raw_h_)


#if defined(play_sd_wav_h_)
// Play from named file
void OSCAudioPlaySdWav::play(OSCMessage& msg)
{
	char buf[50];	
	msg.getString(0,buf,50);
	AudioPlaySdWav::play(buf);
}
#endif // defined(play_sd_wav_h_)


#if defined(play_serial_raw_h_)
// Play from named file
void OSCAudioPlaySerialflashRaw::play(OSCMessage& msg)
{
	char buf[50];	
	msg.getString(0,buf,50);
	AudioPlaySerialflashRaw::play(buf);
}
#endif // defined(play_serial_raw_h_)


#if defined(synth_waveform_h_)
// Set arbitrary waveform from 256-sample (512-byte) blob
bool OSCAudioSynthWaveform::arbitraryWaveform(OSCMessage& msg)
{
	bool result = false;
	float maxFreq = msg.getFloat(1);
	int16_t max_len_def = msg.getBlobLength(0); // actual blob length
	
	if (256 * sizeof *arbdata != max_len_def)
	{
		arbdata = (int16_t*) getSpace((void**) &arbdata,max_len_def,0); // get some storage
		if (NULL != arbdata)
		{
			msg.getBlob(0,(uint8_t*) arbdata,max_len_def);
			AudioSynthWaveform::arbitraryWaveform(arbdata,maxFreq);
			result = true;
		}
	}
	
	return result;
}
#endif // defined(synth_waveform_h_)


#if defined(synth_waveform_h_)
// Set arbitrary waveform from 256-sample (512-byte) blob
bool OSCAudioSynthWaveformModulated::arbitraryWaveform(OSCMessage& msg)
{
	bool result = false;
	float maxFreq = msg.getFloat(1);
	int16_t max_len_def = msg.getBlobLength(0); // actual blob length
	
	if (256 * sizeof *arbdata != max_len_def)
	{
		arbdata = (int16_t*) getSpace((void**) &arbdata,max_len_def,0); // get some storage
		if (NULL != arbdata)
		{
			msg.getBlob(0,(uint8_t*) arbdata,max_len_def);
			AudioSynthWaveformModulated::arbitraryWaveform(arbdata,maxFreq);
			result = true;
		}
	}
	
	return result;
}
#endif // defined(synth_waveform_h_)
