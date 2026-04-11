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

#if !defined(OSC_AUTOGEN_H)
#define OSC_AUTOGEN_H

#if defined(async_input_spdif3_h_)
// ============== AsyncAudioInputSPDIF3 ====================
class AsyncOSCAudioInputSPDIF3 : public AsyncAudioInputSPDIF3, public OSCAudioBase
{
    public:
        AsyncOSCAudioInputSPDIF3(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        AsyncOSCAudioInputSPDIF3(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/b*",NULL)) {begin(); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void begin();
            else if (isTarget(msg,addrOff,"/getA*",NULL)) {addReplyResult(msg,addrOff,reply,(float)getAttenuation(),nameOfTarget); } // double getAttenuation() const;
            else if (isTarget(msg,addrOff,"/getB*",NULL)) {addReplyResult(msg,addrOff,reply,(float)getBufferedTime(),nameOfTarget); } // double getBufferedTime() const;
            else if (isTarget(msg,addrOff,"/getH*",NULL)) {addReplyResult(msg,addrOff,reply,getHalfFilterLength(),nameOfTarget); } // int32_t getHalfFilterLength() const;
            else if (isTarget(msg,addrOff,"/getI*",NULL)) {addReplyResult(msg,addrOff,reply,(float)getInputFrequency(),nameOfTarget); } // double getInputFrequency() const;
            else if (isTarget(msg,addrOff,"/getT*",NULL)) {addReplyResult(msg,addrOff,reply,(float)getTargetLantency(),nameOfTarget); } // double getTargetLantency() const;
            else if (isTarget(msg,addrOff,"/i*",NULL)) {addReplyResult(msg,addrOff,reply,isLocked(),nameOfTarget); } // static bool isLocked();
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_async_input_spdif3_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_async_input_spdif3_h_(a)
#endif // defined(async_input_spdif3_h_)

#if defined(mixer_h_)
// ============== AudioAmplifier ====================
class OSCAudioAmplifier : public AudioAmplifier, public OSCAudioBase
{
    public:
        OSCAudioAmplifier(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioAmplifier(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/g*","f")) {gain(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void gain(float n) {
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_mixer_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_mixer_h_(a)
#endif // defined(mixer_h_)

#if defined(analyze_fft1024_h_)
// ============== AudioAnalyzeFFT1024 ====================
class OSCAudioAnalyzeFFT1024 : public AudioAnalyzeFFT1024, public OSCAudioBase
{
    public:
        OSCAudioAnalyzeFFT1024(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioAnalyzeFFT1024(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/ava*",NULL)) {addReplyResult(msg,addrOff,reply,available(),nameOfTarget); } // bool available() {
            else if (isTarget(msg,addrOff,"/ave*","i")) {averageTogether(msg.getInt(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void averageTogether(uint8_t n) {
            else if (isTarget(msg,addrOff,"/r*","ii")) {addReplyResult(msg,addrOff,reply,read(msg.getInt(0),msg.getInt(1)),nameOfTarget); } // float read(unsigned int binFirst, unsigned int binLast) {
            else if (isTarget(msg,addrOff,"/r*","i")) {addReplyResult(msg,addrOff,reply,read(msg.getInt(0)),nameOfTarget); } // float read(unsigned int binNumber) {
            else if (isTarget(msg,addrOff,"/w*","s")) {addReplyResult(msg,addrOff,reply,windowFunction(msg),nameOfTarget); } // void windowFunction(const int16_t *w) {
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
	private:
		bool windowFunction(OSCMessage& msg);
};
#define OSC_CLASS_analyze_fft1024_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_analyze_fft1024_h_(a)
#endif // defined(analyze_fft1024_h_)

#if defined(analyze_fft256_h_)
// ============== AudioAnalyzeFFT256 ====================
class OSCAudioAnalyzeFFT256 : public AudioAnalyzeFFT256, public OSCAudioBase
{
    public:
        OSCAudioAnalyzeFFT256(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioAnalyzeFFT256(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/ava*",NULL)) {addReplyResult(msg,addrOff,reply,available(),nameOfTarget); } // bool available() {
            else if (isTarget(msg,addrOff,"/ave*","i")) {averageTogether(msg.getInt(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void averageTogether(uint8_t n) {
            else if (isTarget(msg,addrOff,"/r*","ii")) {addReplyResult(msg,addrOff,reply,read(msg.getInt(0),msg.getInt(1)),nameOfTarget); } // float read(unsigned int binFirst, unsigned int binLast) {
            else if (isTarget(msg,addrOff,"/r*","i")) {addReplyResult(msg,addrOff,reply,read(msg.getInt(0)),nameOfTarget); } // float read(unsigned int binNumber) {
            else if (isTarget(msg,addrOff,"/w*","s")) {addReplyResult(msg,addrOff,reply,windowFunction(msg),nameOfTarget); } // void windowFunction(const int16_t *w) {
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
	private:
		bool windowFunction(OSCMessage& msg);
};
#define OSC_CLASS_analyze_fft256_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_analyze_fft256_h_(a)
#endif // defined(analyze_fft256_h_)

#if defined(AudioAnalyzeNoteFrequency_h_)
// ============== AudioAnalyzeNoteFrequency ====================
class OSCAudioAnalyzeNoteFrequency : public AudioAnalyzeNoteFrequency, public OSCAudioBase
{
    public:
        OSCAudioAnalyzeNoteFrequency(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioAnalyzeNoteFrequency(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/a*",NULL)) {addReplyResult(msg,addrOff,reply,available(),nameOfTarget); } // bool available( void );
            else if (isTarget(msg,addrOff,"/b*","f")) {begin(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void begin( float threshold );
            else if (isTarget(msg,addrOff,"/p*",NULL)) {addReplyResult(msg,addrOff,reply,probability(),nameOfTarget); } // float probability( void );
            else if (isTarget(msg,addrOff,"/r*",NULL)) {addReplyResult(msg,addrOff,reply,read(),nameOfTarget); } // float read( void );
            else if (isTarget(msg,addrOff,"/t*","f")) {threshold(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void threshold( float p );
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_AudioAnalyzeNoteFrequency_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_AudioAnalyzeNoteFrequency_h_(a)
#endif // defined(AudioAnalyzeNoteFrequency_h_)

#if defined(analyze_peakdetect_h_)
// ============== AudioAnalyzePeak ====================
class OSCAudioAnalyzePeak : public AudioAnalyzePeak, public OSCAudioBase
{
    public:
        OSCAudioAnalyzePeak(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioAnalyzePeak(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/a*",NULL)) {addReplyResult(msg,addrOff,reply,available(),nameOfTarget); } // bool available(void) {
            else if (isTarget(msg,addrOff,"/readP*",NULL)) {addReplyResult(msg,addrOff,reply,readPeakToPeak(),nameOfTarget); } // float readPeakToPeak(void) {
            else if (isTarget(msg,addrOff,"/read",NULL)) {addReplyResult(msg,addrOff,reply,read(),nameOfTarget); } // float read(void) {
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_analyze_peakdetect_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_analyze_peakdetect_h_(a)
#endif // defined(analyze_peakdetect_h_)

#if defined(analyze_print_h_)
// ============== AudioAnalyzePrint ====================
class OSCAudioAnalyzePrint : public AudioAnalyzePrint, public OSCAudioBase
{
    public:
        OSCAudioAnalyzePrint(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this), namePtr(0) {}
        OSCAudioAnalyzePrint(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this), namePtr(0) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/d*","i")) {delay(msg.getInt(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void delay(uint32_t num) { delay_length = num; }
            else if (isTarget(msg,addrOff,"/l*","i")) {length(msg.getInt(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void length(uint32_t num) { print_length = num; }
            else if (isTarget(msg,addrOff,"/n*","s")) {addReplyResult(msg,addrOff,reply,name(msg),nameOfTarget); } // void name(const char *str) { myname = str; }
            // NOT DEFINED: else if (isTarget(msg,addrOff,"/t*","fi")) {trigger(msg.getFloat(0),msg.getInt(1)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void trigger(float level, int edge);
            else if (isTarget(msg,addrOff,"/t*",NULL)) {trigger(); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void trigger(void);
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
	private:
		bool name(OSCMessage& msg);
		char* namePtr;
};
#define OSC_CLASS_analyze_print_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_analyze_print_h_(a)
#endif // defined(analyze_print_h_)

#if defined(analyze_rms_h_)
// ============== AudioAnalyzeRMS ====================
class OSCAudioAnalyzeRMS : public AudioAnalyzeRMS, public OSCAudioBase
{
    public:
        OSCAudioAnalyzeRMS(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioAnalyzeRMS(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/a*",NULL)) {addReplyResult(msg,addrOff,reply,available(),nameOfTarget); } // bool available(void) {
            else if (isTarget(msg,addrOff,"/r*",NULL)) {addReplyResult(msg,addrOff,reply,read(),nameOfTarget); } // float read(void);
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_analyze_rms_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_analyze_rms_h_(a)
#endif // defined(analyze_rms_h_)

#if defined(analyze_tonedetect_h_)
// ============== AudioAnalyzeToneDetect ====================
class OSCAudioAnalyzeToneDetect : public AudioAnalyzeToneDetect, public OSCAudioBase
{
    public:
        OSCAudioAnalyzeToneDetect(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioAnalyzeToneDetect(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/a*",NULL)) {addReplyResult(msg,addrOff,reply,available(),nameOfTarget); } // bool available(void) {
            else if (isTarget(msg,addrOff,"/f*","fi")) {frequency(msg.getFloat(0),msg.getInt(1)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void frequency(float freq, uint16_t cycles=10) {
            else if (isTarget(msg,addrOff,"/r*",NULL)) {addReplyResult(msg,addrOff,reply,read(),nameOfTarget); } // float read(void);
            else if (isTarget(msg,addrOff,"/s*","iii")) {set_params(msg.getInt(0),msg.getInt(1),msg.getInt(2)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void set_params(int32_t coef, uint16_t cycles, uint16_t len);
            else if (isTarget(msg,addrOff,"/t*","f")) {threshold(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void threshold(float level) {
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_analyze_tonedetect_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_analyze_tonedetect_h_(a)
#endif // defined(analyze_tonedetect_h_)

#if defined(control_ak4558_h_)
// ============== AudioControlAK4558 ====================
class OSCAudioControlAK4558 : public AudioControlAK4558, public OSCAudioBase
{
    public:
        OSCAudioControlAK4558(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioControlAK4558(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/disableI*",NULL)) {addReplyResult(msg,addrOff,reply,disableIn(),nameOfTarget); } // bool disableIn(void);	//powers down ADC
            else if (isTarget(msg,addrOff,"/disableO*",NULL)) {addReplyResult(msg,addrOff,reply,disableOut(),nameOfTarget); } // bool disableOut(void);	//powers down DAC
            else if (isTarget(msg,addrOff,"/disable",NULL)) {addReplyResult(msg,addrOff,reply,disable(),nameOfTarget); } // bool disable(void) { return (disableIn()&&disableOut()); }	//powers down ADC/DAC
            else if (isTarget(msg,addrOff,"/enableI*",NULL)) {addReplyResult(msg,addrOff,reply,enableIn(),nameOfTarget); } // bool enableIn(void);	//powers up ADC
            else if (isTarget(msg,addrOff,"/enableO*",NULL)) {addReplyResult(msg,addrOff,reply,enableOut(),nameOfTarget); } // bool enableOut(void);	//powers up DAC
            else if (isTarget(msg,addrOff,"/enable",NULL)) {addReplyResult(msg,addrOff,reply,enable(),nameOfTarget); } // bool enable(void);		//enables the CODEC, does not power up ADC nor DAC (use enableIn() and enableOut() for selective power up)
            else if (isTarget(msg,addrOff,"/inputL*","f")) {addReplyResult(msg,addrOff,reply,inputLevel(msg.getFloat(0)),nameOfTarget); } // bool inputLevel(float n) { return false; }	//not supported by AK4558
            else if (isTarget(msg,addrOff,"/inputS*","i")) {addReplyResult(msg,addrOff,reply,inputSelect(msg.getInt(0)),nameOfTarget); } // bool inputSelect(int n) { return false; }	//sets inputs to mono left, mono right, stereo (default stereo), not yet implemented
            else if (isTarget(msg,addrOff,"/volumeL*","f")) {addReplyResult(msg,addrOff,reply,volumeLeft(msg.getFloat(0)),nameOfTarget); } // bool volumeLeft(float n);	//sets LOUT volume to n (range 0.0 - 1.0)
            else if (isTarget(msg,addrOff,"/volumeR*","f")) {addReplyResult(msg,addrOff,reply,volumeRight(msg.getFloat(0)),nameOfTarget); } // bool volumeRight(float n);	//sets ROUT volume to n (range 0.0 - 1.0)
            else if (isTarget(msg,addrOff,"/volume","f")) {addReplyResult(msg,addrOff,reply,volume(msg.getFloat(0)),nameOfTarget); } // bool volume(float n);	//sets LOUT/ROUT volume to n (range 0.0 - 1.0)
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_control_ak4558_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_control_ak4558_h_(a)
#endif // defined(control_ak4558_h_)

#if defined(control_cs42448_h_)
// ============== AudioControlCS42448 ====================
class OSCAudioControlCS42448 : public AudioControlCS42448, public OSCAudioBase
{
    public:
        OSCAudioControlCS42448(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioControlCS42448(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/d*",NULL)) {addReplyResult(msg,addrOff,reply,disable(),nameOfTarget); } // bool disable(void) {
            else if (isTarget(msg,addrOff,"/e*",NULL)) {addReplyResult(msg,addrOff,reply,enable(),nameOfTarget); } // bool enable(void);
            else if (isTarget(msg,addrOff,"/f*",NULL)) {addReplyResult(msg,addrOff,reply,filterFreeze(),nameOfTarget); } // bool filterFreeze(void);
            else if (isTarget(msg,addrOff,"/inputL*","f")) {addReplyResult(msg,addrOff,reply,inputLevel(msg.getFloat(0)),nameOfTarget); } // bool inputLevel(float level) {
            else if (isTarget(msg,addrOff,"/inputL*","if")) {addReplyResult(msg,addrOff,reply,inputLevel(msg.getInt(0),msg.getFloat(1)),nameOfTarget); } // bool inputLevel(int channel, float level) {
            else if (isTarget(msg,addrOff,"/inputS*","i")) {addReplyResult(msg,addrOff,reply,inputSelect(msg.getInt(0)),nameOfTarget); } // bool inputSelect(int n) {
            else if (isTarget(msg,addrOff,"/invertA*","i")) {addReplyResult(msg,addrOff,reply,invertADC(msg.getInt(0)),nameOfTarget); } // bool invertADC(uint32_t data);
            else if (isTarget(msg,addrOff,"/invertD*","i")) {addReplyResult(msg,addrOff,reply,invertDAC(msg.getInt(0)),nameOfTarget); } // bool invertDAC(uint32_t data);
            else if (isTarget(msg,addrOff,"/s*","i")) {setAddress(msg.getInt(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void setAddress(uint8_t addr) {
            else if (isTarget(msg,addrOff,"/v*","f")) {addReplyResult(msg,addrOff,reply,volume(msg.getFloat(0)),nameOfTarget); } // bool volume(float level) {
            else if (isTarget(msg,addrOff,"/v*","if")) {addReplyResult(msg,addrOff,reply,volume(msg.getInt(0),msg.getFloat(1)),nameOfTarget); } // bool volume(int channel, float level) {
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_control_cs42448_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_control_cs42448_h_(a)
#endif // defined(control_cs42448_h_)

#if defined(control_cs4272_h_)
// ============== AudioControlCS4272 ====================
class OSCAudioControlCS4272 : public AudioControlCS4272, public OSCAudioBase
{
    public:
        OSCAudioControlCS4272(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioControlCS4272(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/da*","ff")) {addReplyResult(msg,addrOff,reply,dacVolume(msg.getFloat(0),msg.getFloat(1)),nameOfTarget); } // bool dacVolume(float left, float right);
            else if (isTarget(msg,addrOff,"/da*","f")) {addReplyResult(msg,addrOff,reply,dacVolume(msg.getFloat(0)),nameOfTarget); } // bool dacVolume(float n) { return volumeInteger(n * 127 + 0.499f); }
            else if (isTarget(msg,addrOff,"/disableD*",NULL)) {addReplyResult(msg,addrOff,reply,disableDither(),nameOfTarget); } // bool disableDither(void);
            else if (isTarget(msg,addrOff,"/disable",NULL)) {addReplyResult(msg,addrOff,reply,disable(),nameOfTarget); } // bool disable(void) { return false; }
            else if (isTarget(msg,addrOff,"/enableD*",NULL)) {addReplyResult(msg,addrOff,reply,enableDither(),nameOfTarget); } // bool enableDither(void);
            else if (isTarget(msg,addrOff,"/enable",NULL)) {addReplyResult(msg,addrOff,reply,enable(),nameOfTarget); } // bool enable(void);
            else if (isTarget(msg,addrOff,"/inputL*","f")) {addReplyResult(msg,addrOff,reply,inputLevel(msg.getFloat(0)),nameOfTarget); } // bool inputLevel(float n) { return false; }
            else if (isTarget(msg,addrOff,"/inputS*","i")) {addReplyResult(msg,addrOff,reply,inputSelect(msg.getInt(0)),nameOfTarget); } // bool inputSelect(int n) { return false; }
            else if (isTarget(msg,addrOff,"/muteI*",NULL)) {addReplyResult(msg,addrOff,reply,muteInput(),nameOfTarget); } // bool muteInput(void);
            else if (isTarget(msg,addrOff,"/muteO*",NULL)) {addReplyResult(msg,addrOff,reply,muteOutput(),nameOfTarget); } // bool muteOutput(void);
            else if (isTarget(msg,addrOff,"/unmuteI*",NULL)) {addReplyResult(msg,addrOff,reply,unmuteInput(),nameOfTarget); } // bool unmuteInput(void);
            else if (isTarget(msg,addrOff,"/unmuteO*",NULL)) {addReplyResult(msg,addrOff,reply,unmuteOutput(),nameOfTarget); } // bool unmuteOutput(void);
            else if (isTarget(msg,addrOff,"/v*","ff")) {addReplyResult(msg,addrOff,reply,volume(msg.getFloat(0),msg.getFloat(1)),nameOfTarget); } // bool volume(float left, float right);
            else if (isTarget(msg,addrOff,"/v*","f")) {addReplyResult(msg,addrOff,reply,volume(msg.getFloat(0)),nameOfTarget); } // bool volume(float n) { return volumeInteger(n * 127 + 0.499f); }
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_control_cs4272_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_control_cs4272_h_(a)
#endif // defined(control_cs4272_h_)

#if defined(control_sgtl5000_h_)
// ============== AudioControlSGTL5000 ====================
class OSCAudioControlSGTL5000 : public AudioControlSGTL5000, public OSCAudioBase
{
    public:
        OSCAudioControlSGTL5000(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioControlSGTL5000(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/adcHighPassFilterD*",NULL)) {addReplyResult(msg,addrOff,reply,(uint16_t)adcHighPassFilterDisable(),nameOfTarget); } // unsigned short adcHighPassFilterDisable(void);
            else if (isTarget(msg,addrOff,"/adcHighPassFilterE*",NULL)) {addReplyResult(msg,addrOff,reply,(uint16_t)adcHighPassFilterEnable(),nameOfTarget); } // unsigned short adcHighPassFilterEnable(void);
            else if (isTarget(msg,addrOff,"/adcHighPassFilterF*",NULL)) {addReplyResult(msg,addrOff,reply,(uint16_t)adcHighPassFilterFreeze(),nameOfTarget); } // unsigned short adcHighPassFilterFreeze(void);
            else if (isTarget(msg,addrOff,"/audioPo*",NULL)) {addReplyResult(msg,addrOff,reply,(uint16_t)audioPostProcessorEnable(),nameOfTarget); } // unsigned short audioPostProcessorEnable(void);
            else if (isTarget(msg,addrOff,"/audioPre*",NULL)) {addReplyResult(msg,addrOff,reply,(uint16_t)audioPreProcessorEnable(),nameOfTarget); } // unsigned short audioPreProcessorEnable(void);
            else if (isTarget(msg,addrOff,"/audioPro*",NULL)) {addReplyResult(msg,addrOff,reply,(uint16_t)audioProcessorDisable(),nameOfTarget); } // unsigned short audioProcessorDisable(void);
            else if (isTarget(msg,addrOff,"/autoVolumeC*","iiifff")) {addReplyResult(msg,addrOff,reply,(uint16_t)autoVolumeControl(msg.getInt(0),msg.getInt(1),msg.getInt(2),msg.getFloat(3),msg.getFloat(4),msg.getFloat(5)),nameOfTarget); } // unsigned short autoVolumeControl(uint8_t maxGain, uint8_t lbiResponse, uint8_t hardLimit, float threshold, float attack, float decay);
            else if (isTarget(msg,addrOff,"/autoVolumeD*",NULL)) {addReplyResult(msg,addrOff,reply,(uint16_t)autoVolumeDisable(),nameOfTarget); } // unsigned short autoVolumeDisable(void);
            else if (isTarget(msg,addrOff,"/autoVolumeE*",NULL)) {addReplyResult(msg,addrOff,reply,(uint16_t)autoVolumeEnable(),nameOfTarget); } // unsigned short autoVolumeEnable(void);
            else if (isTarget(msg,addrOff,"/dacVolumeRampD*",NULL)) {addReplyResult(msg,addrOff,reply,dacVolumeRampDisable(),nameOfTarget); } // bool dacVolumeRampDisable();
            else if (isTarget(msg,addrOff,"/dacVolumeRampL*",NULL)) {addReplyResult(msg,addrOff,reply,dacVolumeRampLinear(),nameOfTarget); } // bool dacVolumeRampLinear();
            else if (isTarget(msg,addrOff,"/dacVolumeRamp",NULL)) {addReplyResult(msg,addrOff,reply,dacVolumeRamp(),nameOfTarget); } // bool dacVolumeRamp();
            else if (isTarget(msg,addrOff,"/dacVolume","ff")) {addReplyResult(msg,addrOff,reply,(uint16_t)dacVolume(msg.getFloat(0),msg.getFloat(1)),nameOfTarget); } // unsigned short dacVolume(float left, float right);
            else if (isTarget(msg,addrOff,"/dacVolume","f")) {addReplyResult(msg,addrOff,reply,(uint16_t)dacVolume(msg.getFloat(0)),nameOfTarget); } // unsigned short dacVolume(float n);
            else if (isTarget(msg,addrOff,"/di*",NULL)) {addReplyResult(msg,addrOff,reply,disable(),nameOfTarget); } // bool disable(void) { return false; }
            else if (isTarget(msg,addrOff,"/ena*","ii")) {addReplyResult(msg,addrOff,reply,enable(msg.getInt(0),msg.getInt(1)),nameOfTarget); } // bool enable(const unsigned extMCLK, const uint32_t pllFreq = (4096.0l * AUDIO_SAMPLE_RATE_EXACT) ); //With extMCLK > 0, the SGTL acts as Master
            else if (isTarget(msg,addrOff,"/ena*",NULL)) {addReplyResult(msg,addrOff,reply,enable(),nameOfTarget); } // bool enable(void);//For Teensy LC the SGTL acts as master, for all other Teensys as slave.
            else if (isTarget(msg,addrOff,"/enhanceBassD*",NULL)) {addReplyResult(msg,addrOff,reply,(uint16_t)enhanceBassDisable(),nameOfTarget); } // unsigned short enhanceBassDisable(void);
            else if (isTarget(msg,addrOff,"/enhanceBassE*",NULL)) {addReplyResult(msg,addrOff,reply,(uint16_t)enhanceBassEnable(),nameOfTarget); } // unsigned short enhanceBassEnable(void);
            else if (isTarget(msg,addrOff,"/enhanceBass","ff")) {addReplyResult(msg,addrOff,reply,(uint16_t)enhanceBass(msg.getFloat(0),msg.getFloat(1)),nameOfTarget); } // unsigned short enhanceBass(float lr_lev, float bass_lev);
            else if (isTarget(msg,addrOff,"/enhanceBass","ffii")) {addReplyResult(msg,addrOff,reply,(uint16_t)enhanceBass(msg.getFloat(0),msg.getFloat(1),msg.getInt(2),msg.getInt(3)),nameOfTarget); } // unsigned short enhanceBass(float lr_lev, float bass_lev, uint8_t hpf_bypass, uint8_t cutoff);
            else if (isTarget(msg,addrOff,"/eqBands","fffff")) {eqBands(msg.getFloat(0),msg.getFloat(1),msg.getFloat(2),msg.getFloat(3),msg.getFloat(4)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void eqBands(float bass, float mid_bass, float midrange, float mid_treble, float treble);
            else if (isTarget(msg,addrOff,"/eqBands","ff")) {eqBands(msg.getFloat(0),msg.getFloat(1)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void eqBands(float bass, float treble);
            else if (isTarget(msg,addrOff,"/eqBand","if")) {addReplyResult(msg,addrOff,reply,(uint16_t)eqBand(msg.getInt(0),msg.getFloat(1)),nameOfTarget); } // unsigned short eqBand(uint8_t bandNum, float n);
            else if (isTarget(msg,addrOff,"/eqFilterC*","i")) {addReplyResult(msg,addrOff,reply,(uint16_t)eqFilterCount(msg.getInt(0)),nameOfTarget); } // unsigned short eqFilterCount(uint8_t n);
            else if (isTarget(msg,addrOff,"/eqFilter","ib")) {eqFilter(msg); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void eqFilter(uint8_t filterNum, int *filterParameters);
            else if (isTarget(msg,addrOff,"/eqS*","i")) {addReplyResult(msg,addrOff,reply,(uint16_t)eqSelect(msg.getInt(0)),nameOfTarget); } // unsigned short eqSelect(uint8_t n);
            else if (isTarget(msg,addrOff,"/h*","i")) {addReplyResult(msg,addrOff,reply,headphoneSelect(msg.getInt(0)),nameOfTarget); } // bool headphoneSelect(int n) {
            else if (isTarget(msg,addrOff,"/inputL*","f")) {addReplyResult(msg,addrOff,reply,inputLevel(msg.getFloat(0)),nameOfTarget); } // bool inputLevel(float n) {return false;}
            else if (isTarget(msg,addrOff,"/inputS*","i")) {addReplyResult(msg,addrOff,reply,inputSelect(msg.getInt(0)),nameOfTarget); } // bool inputSelect(int n) {
            else if (isTarget(msg,addrOff,"/k*",NULL)) {killAutomation(); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void killAutomation(void) { semi_automated=false; }
            else if (isTarget(msg,addrOff,"/lineI*","ii")) {addReplyResult(msg,addrOff,reply,lineInLevel(msg.getInt(0),msg.getInt(1)),nameOfTarget); } // bool lineInLevel(uint8_t left, uint8_t right);
            else if (isTarget(msg,addrOff,"/lineI*","i")) {addReplyResult(msg,addrOff,reply,lineInLevel(msg.getInt(0)),nameOfTarget); } // bool lineInLevel(uint8_t n) { return lineInLevel(n, n); }
            else if (isTarget(msg,addrOff,"/lineO*","ii")) {addReplyResult(msg,addrOff,reply,(uint16_t)lineOutLevel(msg.getInt(0),msg.getInt(1)),nameOfTarget); } // unsigned short lineOutLevel(uint8_t left, uint8_t right);
            else if (isTarget(msg,addrOff,"/lineO*","i")) {addReplyResult(msg,addrOff,reply,(uint16_t)lineOutLevel(msg.getInt(0)),nameOfTarget); } // unsigned short lineOutLevel(uint8_t n);
            else if (isTarget(msg,addrOff,"/mi*","i")) {addReplyResult(msg,addrOff,reply,micGain(msg.getInt(0)),nameOfTarget); } // bool micGain(unsigned int dB);
            else if (isTarget(msg,addrOff,"/muteH*",NULL)) {addReplyResult(msg,addrOff,reply,muteHeadphone(),nameOfTarget); } // bool muteHeadphone(void) { return write(0x0024, ana_ctrl | (1<<4)); }
            else if (isTarget(msg,addrOff,"/muteL*",NULL)) {addReplyResult(msg,addrOff,reply,muteLineout(),nameOfTarget); } // bool muteLineout(void) { return write(0x0024, ana_ctrl | (1<<8)); }
            else if (isTarget(msg,addrOff,"/se*","i")) {setAddress(msg.getInt(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void setAddress(uint8_t level);
            else if (isTarget(msg,addrOff,"/surroundSoundD*",NULL)) {addReplyResult(msg,addrOff,reply,(uint16_t)surroundSoundDisable(),nameOfTarget); } // unsigned short surroundSoundDisable(void);
            else if (isTarget(msg,addrOff,"/surroundSoundE*",NULL)) {addReplyResult(msg,addrOff,reply,(uint16_t)surroundSoundEnable(),nameOfTarget); } // unsigned short surroundSoundEnable(void);
            else if (isTarget(msg,addrOff,"/surroundSound","i")) {addReplyResult(msg,addrOff,reply,(uint16_t)surroundSound(msg.getInt(0)),nameOfTarget); } // unsigned short surroundSound(uint8_t width);
            else if (isTarget(msg,addrOff,"/surroundSound","ii")) {addReplyResult(msg,addrOff,reply,(uint16_t)surroundSound(msg.getInt(0),msg.getInt(1)),nameOfTarget); } // unsigned short surroundSound(uint8_t width, uint8_t select);
            else if (isTarget(msg,addrOff,"/unmuteH*",NULL)) {addReplyResult(msg,addrOff,reply,unmuteHeadphone(),nameOfTarget); } // bool unmuteHeadphone(void) { return write(0x0024, ana_ctrl & ~(1<<4)); }
            else if (isTarget(msg,addrOff,"/unmuteL*",NULL)) {addReplyResult(msg,addrOff,reply,unmuteLineout(),nameOfTarget); } // bool unmuteLineout(void) { return write(0x0024, ana_ctrl & ~(1<<8)); }
            else if (isTarget(msg,addrOff,"/v*","ff")) {addReplyResult(msg,addrOff,reply,volume(msg.getFloat(0),msg.getFloat(1)),nameOfTarget); } // bool volume(float left, float right);
            else if (isTarget(msg,addrOff,"/v*","f")) {addReplyResult(msg,addrOff,reply,volume(msg.getFloat(0)),nameOfTarget); } // bool volume(float n) { return volumeInteger(n * 129 + 0.499f); }
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
	private:
		void eqFilter(OSCMessage& msg);
};
#define OSC_CLASS_control_sgtl5000_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_control_sgtl5000_h_(a)
#endif // defined(control_sgtl5000_h_)

#if defined(control_tlv320aic3206_h_)
// ============== AudioControlTLV320AIC3206 ====================
class OSCAudioControlTLV320AIC3206 : public AudioControlTLV320AIC3206, public OSCAudioBase
{
    public:
        OSCAudioControlTLV320AIC3206(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioControlTLV320AIC3206(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/aic_r*","ii")) {addReplyResult(msg,addrOff,reply,(uint32_t)aic_readPage(msg.getInt(0),msg.getInt(1)),nameOfTarget); } // unsigned int aic_readPage(uint8_t page, uint8_t reg);
            else if (isTarget(msg,addrOff,"/aic_w*","iii")) {addReplyResult(msg,addrOff,reply,aic_writePage(msg.getInt(0),msg.getInt(1),msg.getInt(2)),nameOfTarget); } // bool aic_writePage(uint8_t page, uint8_t reg, uint8_t val);
            else if (isTarget(msg,addrOff,"/d*",NULL)) {addReplyResult(msg,addrOff,reply,disable(),nameOfTarget); } // bool disable(void);
            else if (isTarget(msg,addrOff,"/enableA*",";i")) {addReplyResult(msg,addrOff,reply,enableAutoMuteDAC(msg.getBoolean(0),msg.getInt(1)),nameOfTarget); } // bool enableAutoMuteDAC(bool, uint8_t);
            else if (isTarget(msg,addrOff,"/enableM*",";")) {addReplyResult(msg,addrOff,reply,enableMicDetect(msg.getBoolean(0)),nameOfTarget); } // bool enableMicDetect(bool);
            else if (isTarget(msg,addrOff,"/enable",NULL)) {addReplyResult(msg,addrOff,reply,enable(),nameOfTarget); } // bool enable(void);
            else if (isTarget(msg,addrOff,"/getH*",NULL)) {addReplyResult(msg,addrOff,reply,getHPCutoff_Hz(),nameOfTarget); } // float getHPCutoff_Hz(void) { return HP_cutoff_Hz; }
            else if (isTarget(msg,addrOff,"/getS*",NULL)) {addReplyResult(msg,addrOff,reply,getSampleRate_Hz(),nameOfTarget); } // float getSampleRate_Hz(void) { return sample_rate_Hz; }
            else if (isTarget(msg,addrOff,"/inputL*","f")) {addReplyResult(msg,addrOff,reply,inputLevel(msg.getFloat(0)),nameOfTarget); } // bool inputLevel(float n);  //dummy to be compatible with Teensy Audio Library
            else if (isTarget(msg,addrOff,"/inputS*","i")) {addReplyResult(msg,addrOff,reply,inputSelect(msg.getInt(0)),nameOfTarget); } // bool inputSelect(int n);   //use AIC3206_INPUT_IN1 or one of other choices defined earlier
            else if (isTarget(msg,addrOff,"/o*","i")) {addReplyResult(msg,addrOff,reply,outputSelect(msg.getInt(0)),nameOfTarget); } // bool outputSelect(int n);  //use AIC3206_OUTPUT_HEADPHONE_JACK_OUT or one of other choices defined earlier
            else if (isTarget(msg,addrOff,"/r*",NULL)) {addReplyResult(msg,addrOff,reply,(int32_t)readMicDetect(),nameOfTarget); } // int  readMicDetect(void);
            else if (isTarget(msg,addrOff,"/setH*",";ff")) {setHPFonADC(msg.getBoolean(0),msg.getFloat(1),msg.getFloat(2)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void setHPFonADC(bool enable, float cutoff_Hz, float fs_Hz);
            else if (isTarget(msg,addrOff,"/setII*","ib")) {setIIRCoeffOnADC(msg); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void setIIRCoeffOnADC(int chan, uint32_t *coeff);  //for chan, use AIC3206_BOTH_CHAN or AIC3206_LEFT_CHAN or AIC3206_RIGHT_CHAN
            else if (isTarget(msg,addrOff,"/setIn*","f")) {addReplyResult(msg,addrOff,reply,setInputGain_dB(msg.getFloat(0)),nameOfTarget); } // bool setInputGain_dB(float n);
            else if (isTarget(msg,addrOff,"/setM*","i")) {addReplyResult(msg,addrOff,reply,setMicBias(msg.getInt(0)),nameOfTarget); } // bool setMicBias(int n);  //use AIC3206_MIC_BIAS_OFF or AIC3206_MIC_BIAS_2_5 or one of other choices defined earlier
            else if (isTarget(msg,addrOff,"/u*","i")) {addReplyResult(msg,addrOff,reply,updateInputBasedOnMicDetect(msg.getInt(0)),nameOfTarget); } // bool updateInputBasedOnMicDetect(int setting = AIC3206_INPUT_IN1); //which input to monitor
            else if (isTarget(msg,addrOff,"/volume_*","f")) {addReplyResult(msg,addrOff,reply,volume_dB(msg.getFloat(0)),nameOfTarget); } // bool volume_dB(float n);
            else if (isTarget(msg,addrOff,"/volume","f")) {addReplyResult(msg,addrOff,reply,volume(msg.getFloat(0)),nameOfTarget); } // bool volume(float n);
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
	private:
		void setIIRCoeffOnADC(OSCMessage& msg);
};
#define OSC_CLASS_control_tlv320aic3206_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_control_tlv320aic3206_h_(a)
#endif // defined(control_tlv320aic3206_h_)

#if defined(control_wm8731_h_)
// ============== AudioControlWM8731 ====================
class OSCAudioControlWM8731 : public AudioControlWM8731, public OSCAudioBase
{
    public:
        OSCAudioControlWM8731(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioControlWM8731(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/d*",NULL)) {addReplyResult(msg,addrOff,reply,disable(),nameOfTarget); } // bool disable(void) { return false; }
            else if (isTarget(msg,addrOff,"/e*",NULL)) {addReplyResult(msg,addrOff,reply,enable(),nameOfTarget); } // bool enable(void);
            else if (isTarget(msg,addrOff,"/inputL*","f")) {addReplyResult(msg,addrOff,reply,inputLevel(msg.getFloat(0)),nameOfTarget); } // bool inputLevel(float n); // range: 0.0f to 1.0f
            else if (isTarget(msg,addrOff,"/inputS*","i")) {addReplyResult(msg,addrOff,reply,inputSelect(msg.getInt(0)),nameOfTarget); } // bool inputSelect(int n);
            else if (isTarget(msg,addrOff,"/v*","f")) {addReplyResult(msg,addrOff,reply,volume(msg.getFloat(0)),nameOfTarget); } // bool volume(float n) { return volumeInteger(n * 80.0f + 47.499f); }
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_control_wm8731_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_control_wm8731_h_(a)
#endif // defined(control_wm8731_h_)

#if defined(control_wm8731_h_)
// ============== AudioControlWM8731master ====================
class OSCAudioControlWM8731master : public AudioControlWM8731master, public OSCAudioBase
{
    public:
        OSCAudioControlWM8731master(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioControlWM8731master(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/e*",NULL)) {addReplyResult(msg,addrOff,reply,enable(),nameOfTarget); } // bool enable(void);
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_control_wm8731_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_control_wm8731_h_(a)
#endif // defined(control_wm8731_h_)

#if defined(effect_bitcrusher_h_)
// ============== AudioEffectBitcrusher ====================
class OSCAudioEffectBitcrusher : public AudioEffectBitcrusher, public OSCAudioBase
{
    public:
        OSCAudioEffectBitcrusher(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioEffectBitcrusher(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/b*","i")) {bits(msg.getInt(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void bits(uint8_t b) {
            else if (isTarget(msg,addrOff,"/s*","f")) {sampleRate(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void sampleRate(float hz) {
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_effect_bitcrusher_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_effect_bitcrusher_h_(a)
#endif // defined(effect_bitcrusher_h_)

#if defined(effect_delay_h_)
// ============== AudioEffectDelay ====================
class OSCAudioEffectDelay : public AudioEffectDelay, public OSCAudioBase
{
    public:
        OSCAudioEffectDelay(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioEffectDelay(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/de*","if")) {delay(msg.getInt(0),msg.getFloat(1)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void delay(uint8_t channel, float milliseconds) {
            else if (isTarget(msg,addrOff,"/di*","i")) {disable(msg.getInt(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void disable(uint8_t channel) {
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_effect_delay_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_effect_delay_h_(a)
#endif // defined(effect_delay_h_)

#if defined(effect_delay_ext_h_)
// ============== AudioEffectDelayExternal ====================
class OSCAudioEffectDelayExternal : public AudioEffectDelayExternal, public OSCAudioBase
{
    public:
        OSCAudioEffectDelayExternal(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioEffectDelayExternal(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/de*","if")) {delay(msg.getInt(0),msg.getFloat(1)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void delay(uint8_t channel, float milliseconds) {
            else if (isTarget(msg,addrOff,"/di*","i")) {disable(msg.getInt(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void disable(uint8_t channel) {
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_effect_delay_ext_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_effect_delay_ext_h_(a)
#endif // defined(effect_delay_ext_h_)

#if defined(effect_digital_combine_h_)
// ============== AudioEffectDigitalCombine ====================
class OSCAudioEffectDigitalCombine : public AudioEffectDigitalCombine, public OSCAudioBase
{
    public:
        OSCAudioEffectDigitalCombine(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioEffectDigitalCombine(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/s*","i")) {setCombineMode(msg.getInt(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void setCombineMode(int mode_in) {
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_effect_digital_combine_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_effect_digital_combine_h_(a)
#endif // defined(effect_digital_combine_h_)

#if defined(effect_envelope_h_)
// ============== AudioEffectEnvelope ====================
class OSCAudioEffectEnvelope : public AudioEffectEnvelope, public OSCAudioBase
{
    public:
        OSCAudioEffectEnvelope(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioEffectEnvelope(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/a*","f")) {attack(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void attack(float milliseconds) {
            else if (isTarget(msg,addrOff,"/dec*","f")) {decay(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void decay(float milliseconds) {
            else if (isTarget(msg,addrOff,"/del*","f")) {delay(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void delay(float milliseconds) {
            else if (isTarget(msg,addrOff,"/h*","f")) {hold(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void hold(float milliseconds) {
            else if (isTarget(msg,addrOff,"/isA*",NULL)) {addReplyResult(msg,addrOff,reply,isActive(),nameOfTarget); } // bool isActive();
            else if (isTarget(msg,addrOff,"/isS*",NULL)) {addReplyResult(msg,addrOff,reply,isSustain(),nameOfTarget); } // bool isSustain();
            else if (isTarget(msg,addrOff,"/noteOf*",NULL)) {noteOff(); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void noteOff();
            else if (isTarget(msg,addrOff,"/noteOn",NULL)) {noteOn(); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void noteOn();
            else if (isTarget(msg,addrOff,"/releaseN*","f")) {releaseNoteOn(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void releaseNoteOn(float milliseconds) {
            else if (isTarget(msg,addrOff,"/release","f")) {release(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void release(float milliseconds) {
            else if (isTarget(msg,addrOff,"/s*","f")) {sustain(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void sustain(float level) {
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_effect_envelope_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_effect_envelope_h_(a)
#endif // defined(effect_envelope_h_)

#if defined(effect_fade_h_)
// ============== AudioEffectFade ====================
class OSCAudioEffectFade : public AudioEffectFade, public OSCAudioBase
{
    public:
        OSCAudioEffectFade(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioEffectFade(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/fadeI*","i")) {fadeIn(msg.getInt(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void fadeIn(uint32_t milliseconds) {
            else if (isTarget(msg,addrOff,"/fadeO*","i")) {fadeOut(msg.getInt(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void fadeOut(uint32_t milliseconds) {
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_effect_fade_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_effect_fade_h_(a)
#endif // defined(effect_fade_h_)

#if defined(effect_freeverb_h_)
// ============== AudioEffectFreeverb ====================
class OSCAudioEffectFreeverb : public AudioEffectFreeverb, public OSCAudioBase
{
    public:
        OSCAudioEffectFreeverb(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioEffectFreeverb(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/d*","f")) {damping(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void damping(float n) {
            else if (isTarget(msg,addrOff,"/r*","f")) {roomsize(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void roomsize(float n) {
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_effect_freeverb_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_effect_freeverb_h_(a)
#endif // defined(effect_freeverb_h_)

#if defined(effect_freeverb_h_)
// ============== AudioEffectFreeverbStereo ====================
class OSCAudioEffectFreeverbStereo : public AudioEffectFreeverbStereo, public OSCAudioBase
{
    public:
        OSCAudioEffectFreeverbStereo(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioEffectFreeverbStereo(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/d*","f")) {damping(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void damping(float n) {
            else if (isTarget(msg,addrOff,"/r*","f")) {roomsize(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void roomsize(float n) {
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_effect_freeverb_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_effect_freeverb_h_(a)
#endif // defined(effect_freeverb_h_)

// ============== AudioEffectGranular ====================
class OSCAudioEffectGranular : public AudioEffectGranular, public OSCAudioBase
{
    public:
        OSCAudioEffectGranular(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this), sample_bank(0) {}
        OSCAudioEffectGranular(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this), sample_bank(0) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/beginF*","f")) {beginFreeze(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void beginFreeze(float grain_length) {
            else if (isTarget(msg,addrOff,"/beginP*","f")) {beginPitchShift(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void beginPitchShift(float grain_length) {
            else if (isTarget(msg,addrOff,"/begin","bi")) {begin(msg); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void begin(int16_t *sample_bank_def, int16_t max_len_def);
            else if (isTarget(msg,addrOff,"/se*","f")) {setSpeed(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void setSpeed(float ratio) {
            else if (isTarget(msg,addrOff,"/st*",NULL)) {stop(); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void stop();
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
	private:
		void begin(OSCMessage& msg);
		int16_t* sample_bank;
};

#if defined(effect_midside_decode_h_)
// ============== AudioEffectMidSide ====================
class OSCAudioEffectMidSide : public AudioEffectMidSide, public OSCAudioBase
{
    public:
        OSCAudioEffectMidSide(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioEffectMidSide(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/d*",NULL)) {decode(); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void decode() { encoding = false; }
            else if (isTarget(msg,addrOff,"/e*",NULL)) {encode(); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void encode() { encoding = true; }
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_effect_midside_decode_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_effect_midside_decode_h_(a)
#endif // defined(effect_midside_decode_h_)

#if defined(effect_multiply_h_)
// ============== AudioEffectMultiply ====================
class OSCAudioEffectMultiply : public AudioEffectMultiply, public OSCAudioBase
{
    public:
        OSCAudioEffectMultiply(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioEffectMultiply(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_effect_multiply_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_effect_multiply_h_(a)
#endif // defined(effect_multiply_h_)

#if defined(effect_rectifier_h_)
// ============== AudioEffectRectifier ====================
class OSCAudioEffectRectifier : public AudioEffectRectifier, public OSCAudioBase
{
    public:
        OSCAudioEffectRectifier(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioEffectRectifier(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_effect_rectifier_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_effect_rectifier_h_(a)
#endif // defined(effect_rectifier_h_)

#if defined(effect_reverb_)
// ============== AudioEffectReverb ====================
class OSCAudioEffectReverb : public AudioEffectReverb, public OSCAudioBase
{
    public:
        OSCAudioEffectReverb(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioEffectReverb(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/r*","f")) {reverbTime(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void reverbTime(float);
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_effect_reverb_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_effect_reverb_(a)
#endif // defined(effect_reverb_)

#if defined(effect_wavefolder_h_)
// ============== AudioEffectWaveFolder ====================
class OSCAudioEffectWaveFolder : public AudioEffectWaveFolder, public OSCAudioBase
{
    public:
        OSCAudioEffectWaveFolder(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioEffectWaveFolder(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_effect_wavefolder_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_effect_wavefolder_h_(a)
#endif // defined(effect_wavefolder_h_)

#if defined(effect_waveshaper_h_)
// ============== AudioEffectWaveshaper ====================
class OSCAudioEffectWaveshaper : public AudioEffectWaveshaper, public OSCAudioBase
{
    public:
        OSCAudioEffectWaveshaper(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioEffectWaveshaper(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            // if (isTarget(msg,addrOff,"/s*","bi")) {shape(msg.getBlob(0),msg.getInt(1)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void shape(float* waveshape, int length);
            addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_effect_waveshaper_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_effect_waveshaper_h_(a)
#endif // defined(effect_waveshaper_h_)

#if defined(filter_biquad_h_)
// ============== AudioFilterBiquad ====================
class OSCAudioFilterBiquad : public AudioFilterBiquad, public OSCAudioBase
{
    public:
        OSCAudioFilterBiquad(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioFilterBiquad(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/setB*","iff")) {setBandpass(msg.getInt(0),msg.getFloat(1),msg.getFloat(2)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void setBandpass(uint32_t stage, float frequency, float q = 1.0) {
            else if (isTarget(msg,addrOff,"/setC*","iddddd")) {setCoefficients(msg); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void setCoefficients(uint32_t stage, const double *coefficients) {
            else if (isTarget(msg,addrOff,"/setC*","iiiiii")) {setCoefficients(msg); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void setCoefficients(uint32_t stage, const int *coefficients);
            else if (isTarget(msg,addrOff,"/setHighS*","ifff")) {setHighShelf(msg.getInt(0),msg.getFloat(1),msg.getFloat(2),msg.getFloat(3)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void setHighShelf(uint32_t stage, float frequency, float gain, float slope = 1.0f) {
            else if (isTarget(msg,addrOff,"/setHighp*","iff")) {setHighpass(msg.getInt(0),msg.getFloat(1),msg.getFloat(2)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void setHighpass(uint32_t stage, float frequency, float q = 0.7071) {
            else if (isTarget(msg,addrOff,"/setLowS*","ifff")) {setLowShelf(msg.getInt(0),msg.getFloat(1),msg.getFloat(2),msg.getFloat(3)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void setLowShelf(uint32_t stage, float frequency, float gain, float slope = 1.0f) {
            else if (isTarget(msg,addrOff,"/setLowp*","iff")) {setLowpass(msg.getInt(0),msg.getFloat(1),msg.getFloat(2)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void setLowpass(uint32_t stage, float frequency, float q = 0.7071f) {
            else if (isTarget(msg,addrOff,"/setN*","iff")) {setNotch(msg.getInt(0),msg.getFloat(1),msg.getFloat(2)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void setNotch(uint32_t stage, float frequency, float q = 1.0) {
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
	private:
		void setCoefficients(OSCMessage& msg);
};
#define OSC_CLASS_filter_biquad_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_filter_biquad_h_(a)
#endif // defined(filter_biquad_h_)

#if defined(filter_fir_h_)
// ============== AudioFilterFIR ====================
class OSCAudioFilterFIR : public AudioFilterFIR, public OSCAudioBase
{
    public:
        OSCAudioFilterFIR(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioFilterFIR(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            // if (isTarget(msg,addrOff,"/b*","bi")) {begin(msg.getBlob(0),msg.getInt(1)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void begin(const short *cp, int n_coeffs) {
            if (isTarget(msg,addrOff,"/e*",NULL)) {end(); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void end(void) {
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_filter_fir_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_filter_fir_h_(a)
#endif // defined(filter_fir_h_)

#if defined(filter_ladder_h_)
// ============== AudioFilterLadder ====================
class OSCAudioFilterLadder : public AudioFilterLadder, public OSCAudioBase
{
    public:
        OSCAudioFilterLadder(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioFilterLadder(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/f*","f")) {frequency(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void frequency(float FC);
            else if (isTarget(msg,addrOff,"/inp*","f")) {inputDrive(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void inputDrive(float drv);
            else if (isTarget(msg,addrOff,"/int*","i")) {interpolationMethod((AudioFilterLadderInterpolation) msg.getInt(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void interpolationMethod(AudioFilterLadderInterpolation im);
            else if (isTarget(msg,addrOff,"/o*","f")) {octaveControl(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void octaveControl(float octaves);
            else if (isTarget(msg,addrOff,"/p*","f")) {passbandGain(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void passbandGain(float passbandgain);
            else if (isTarget(msg,addrOff,"/r*","f")) {resonance(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void resonance(float reson);
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_filter_ladder_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_filter_ladder_h_(a)
#endif // defined(filter_ladder_h_)

#if defined(filter_variable_h_)
// ============== AudioFilterStateVariable ====================
class OSCAudioFilterStateVariable : public AudioFilterStateVariable, public OSCAudioBase
{
    public:
        OSCAudioFilterStateVariable(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioFilterStateVariable(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/f*","f")) {frequency(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void frequency(float freq) {
            else if (isTarget(msg,addrOff,"/o*","f")) {octaveControl(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void octaveControl(float n) {
            else if (isTarget(msg,addrOff,"/r*","f")) {resonance(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void resonance(float q) {
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_filter_variable_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_filter_variable_h_(a)
#endif // defined(filter_variable_h_)

#if defined(input_adc_h_)
// ============== AudioInputAnalog ====================
class OSCAudioInputAnalog : public AudioInputAnalog, public OSCAudioBase
{
    public:
        OSCAudioInputAnalog(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioInputAnalog(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_input_adc_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_input_adc_h_(a)
#endif // defined(input_adc_h_)

#if defined(input_adcs_h_)
// ============== AudioInputAnalogStereo ====================
class OSCAudioInputAnalogStereo : public AudioInputAnalogStereo, public OSCAudioBase
{
    public:
        OSCAudioInputAnalogStereo(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioInputAnalogStereo(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_input_adcs_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_input_adcs_h_(a)
#endif // defined(input_adcs_h_)

#if defined(_input_i2s_h_)
// ============== AudioInputI2S ====================
class OSCAudioInputI2S : public AudioInputI2S, public OSCAudioBase
{
    public:
        OSCAudioInputI2S(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioInputI2S(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS__input_i2s_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS__input_i2s_h_(a)
#endif // defined(_input_i2s_h_)

#if defined(_input_i2s2_h_)
// ============== AudioInputI2S2 ====================
class OSCAudioInputI2S2 : public AudioInputI2S2, public OSCAudioBase
{
    public:
        OSCAudioInputI2S2(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioInputI2S2(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS__input_i2s2_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS__input_i2s2_h_(a)
#endif // defined(_input_i2s2_h_)

#if defined(_input_i2s_hex_h_)
// ============== AudioInputI2SHex ====================
class OSCAudioInputI2SHex : public AudioInputI2SHex, public OSCAudioBase
{
    public:
        OSCAudioInputI2SHex(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioInputI2SHex(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS__input_i2s_hex_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS__input_i2s_hex_h_(a)
#endif // defined(_input_i2s_hex_h_)

#if defined(_input_i2s_oct_h_)
// ============== AudioInputI2SOct ====================
class OSCAudioInputI2SOct : public AudioInputI2SOct, public OSCAudioBase
{
    public:
        OSCAudioInputI2SOct(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioInputI2SOct(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS__input_i2s_oct_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS__input_i2s_oct_h_(a)
#endif // defined(_input_i2s_oct_h_)

#if defined(_input_i2s_quad_h_)
// ============== AudioInputI2SQuad ====================
class OSCAudioInputI2SQuad : public AudioInputI2SQuad, public OSCAudioBase
{
    public:
        OSCAudioInputI2SQuad(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioInputI2SQuad(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS__input_i2s_quad_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS__input_i2s_quad_h_(a)
#endif // defined(_input_i2s_quad_h_)

#if defined(_input_pdm_h_)
// ============== AudioInputPDM ====================
class OSCAudioInputPDM : public AudioInputPDM, public OSCAudioBase
{
    public:
        OSCAudioInputPDM(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioInputPDM(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS__input_pdm_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS__input_pdm_h_(a)
#endif // defined(_input_pdm_h_)

#if defined(_input_pdm_i2s2_h_)
// ============== AudioInputPDM2 ====================
class OSCAudioInputPDM2 : public AudioInputPDM2, public OSCAudioBase
{
    public:
        OSCAudioInputPDM2(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioInputPDM2(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS__input_pdm_i2s2_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS__input_pdm_i2s2_h_(a)
#endif // defined(_input_pdm_i2s2_h_)

#if defined(_input_spdif3_h_)
// ============== AudioInputSPDIF3 ====================
class OSCAudioInputSPDIF3 : public AudioInputSPDIF3, public OSCAudioBase
{
    public:
        OSCAudioInputSPDIF3(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioInputSPDIF3(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/p*",NULL)) {addReplyResult(msg,addrOff,reply,pllLocked(),nameOfTarget); } // static bool pllLocked(void);
            else if (isTarget(msg,addrOff,"/s*",NULL)) {addReplyResult(msg,addrOff,reply,(uint32_t)sampleRate(),nameOfTarget); } // static unsigned int sampleRate(void);
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS__input_spdif3_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS__input_spdif3_h_(a)
#endif // defined(_input_spdif3_h_)

#if defined(_input_tdm_h_)
// ============== AudioInputTDM ====================
class OSCAudioInputTDM : public AudioInputTDM, public OSCAudioBase
{
    public:
        OSCAudioInputTDM(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioInputTDM(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS__input_tdm_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS__input_tdm_h_(a)
#endif // defined(_input_tdm_h_)

#if defined(_input_tdm2_h_)
// ============== AudioInputTDM2 ====================
class OSCAudioInputTDM2 : public AudioInputTDM2, public OSCAudioBase
{
    public:
        OSCAudioInputTDM2(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioInputTDM2(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS__input_tdm2_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS__input_tdm2_h_(a)
#endif // defined(_input_tdm2_h_)

// ============== AudioInputUSB ====================
class OSCAudioInputUSB : public AudioInputUSB, public OSCAudioBase
{
    public:
        OSCAudioInputUSB(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioInputUSB(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/v*",NULL)) {addReplyResult(msg,addrOff,reply,volume(),nameOfTarget); } // float volume(void) {
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};

#if defined(mixer_h_)
// ============== AudioMixer4 ====================
class OSCAudioMixer4 : public AudioMixer4, public OSCAudioBase
{
    public:
        OSCAudioMixer4(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioMixer4(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/g*","if")) {gain(msg.getInt(0),msg.getFloat(1)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void gain(unsigned int channel, float gain) {
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_mixer_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_mixer_h_(a)
#endif // defined(mixer_h_)

#if defined(output_ADAT_h_)
// ============== AudioOutputADAT ====================
class OSCAudioOutputADAT : public AudioOutputADAT, public OSCAudioBase
{
    public:
        OSCAudioOutputADAT(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioOutputADAT(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/m*",";")) {mute_PCM(msg.getBoolean(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // static void mute_PCM(const bool mute);
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_output_ADAT_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_output_ADAT_h_(a)
#endif // defined(output_ADAT_h_)

#if defined(output_dac_h_)
// ============== AudioOutputAnalog ====================
class OSCAudioOutputAnalog : public AudioOutputAnalog, public OSCAudioBase
{
    public:
        OSCAudioOutputAnalog(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioOutputAnalog(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/a*","i")) {analogReference(msg.getInt(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void analogReference(int ref);
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_output_dac_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_output_dac_h_(a)
#endif // defined(output_dac_h_)

#if defined(output_dacs_h_)
// ============== AudioOutputAnalogStereo ====================
class OSCAudioOutputAnalogStereo : public AudioOutputAnalogStereo, public OSCAudioBase
{
    public:
        OSCAudioOutputAnalogStereo(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioOutputAnalogStereo(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/a*","i")) {analogReference(msg.getInt(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void analogReference(int ref);
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_output_dacs_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_output_dacs_h_(a)
#endif // defined(output_dacs_h_)

#if defined(output_i2s_h_)
// ============== AudioOutputI2S ====================
class OSCAudioOutputI2S : public AudioOutputI2S, public OSCAudioBase
{
    public:
        OSCAudioOutputI2S(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioOutputI2S(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_output_i2s_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_output_i2s_h_(a)
#endif // defined(output_i2s_h_)

#if defined(output_i2s2_h_)
// ============== AudioOutputI2S2 ====================
class OSCAudioOutputI2S2 : public AudioOutputI2S2, public OSCAudioBase
{
    public:
        OSCAudioOutputI2S2(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioOutputI2S2(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_output_i2s2_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_output_i2s2_h_(a)
#endif // defined(output_i2s2_h_)

#if defined(output_i2s_hex_h_)
// ============== AudioOutputI2SHex ====================
class OSCAudioOutputI2SHex : public AudioOutputI2SHex, public OSCAudioBase
{
    public:
        OSCAudioOutputI2SHex(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioOutputI2SHex(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_output_i2s_hex_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_output_i2s_hex_h_(a)
#endif // defined(output_i2s_hex_h_)

#if defined(output_i2s_oct_h_)
// ============== AudioOutputI2SOct ====================
class OSCAudioOutputI2SOct : public AudioOutputI2SOct, public OSCAudioBase
{
    public:
        OSCAudioOutputI2SOct(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioOutputI2SOct(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_output_i2s_oct_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_output_i2s_oct_h_(a)
#endif // defined(output_i2s_oct_h_)

#if defined(output_i2s_quad_h_)
// ============== AudioOutputI2SQuad ====================
class OSCAudioOutputI2SQuad : public AudioOutputI2SQuad, public OSCAudioBase
{
    public:
        OSCAudioOutputI2SQuad(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioOutputI2SQuad(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_output_i2s_quad_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_output_i2s_quad_h_(a)
#endif // defined(output_i2s_quad_h_)

#if defined(output_mqs_h_)
// ============== AudioOutputMQS ====================
class OSCAudioOutputMQS : public AudioOutputMQS, public OSCAudioBase
{
    public:
        OSCAudioOutputMQS(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioOutputMQS(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_output_mqs_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_output_mqs_h_(a)
#endif // defined(output_mqs_h_)

#if defined(output_pt8211_h_)
// ============== AudioOutputPT8211 ====================
class OSCAudioOutputPT8211 : public AudioOutputPT8211, public OSCAudioBase
{
    public:
        OSCAudioOutputPT8211(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioOutputPT8211(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_output_pt8211_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_output_pt8211_h_(a)
#endif // defined(output_pt8211_h_)

#if defined(output_pt8211_2_h_)
// ============== AudioOutputPT8211_2 ====================
class OSCAudioOutputPT8211_2 : public AudioOutputPT8211_2, public OSCAudioBase
{
    public:
        OSCAudioOutputPT8211_2(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioOutputPT8211_2(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_output_pt8211_2_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_output_pt8211_2_h_(a)
#endif // defined(output_pt8211_2_h_)

#if defined(output_pwm_h_)
// ============== AudioOutputPWM ====================
class OSCAudioOutputPWM : public AudioOutputPWM, public OSCAudioBase
{
    public:
        OSCAudioOutputPWM(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioOutputPWM(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_output_pwm_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_output_pwm_h_(a)
#endif // defined(output_pwm_h_)

#if defined(output_SPDIF_h_)
// ============== AudioOutputSPDIF ====================
class OSCAudioOutputSPDIF : public AudioOutputSPDIF, public OSCAudioBase
{
    public:
        OSCAudioOutputSPDIF(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioOutputSPDIF(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/m*",";")) {mute_PCM(msg.getBoolean(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // static void mute_PCM(const bool mute);
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_output_SPDIF_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_output_SPDIF_h_(a)
#endif // defined(output_SPDIF_h_)

#if defined(output_SPDIF2_h_)
// ============== AudioOutputSPDIF2 ====================
class OSCAudioOutputSPDIF2 : public AudioOutputSPDIF2, public OSCAudioBase
{
    public:
        OSCAudioOutputSPDIF2(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioOutputSPDIF2(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/m*",";")) {mute_PCM(msg.getBoolean(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // static void mute_PCM(const bool mute);
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_output_SPDIF2_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_output_SPDIF2_h_(a)
#endif // defined(output_SPDIF2_h_)

#if defined(output_SPDIF3_h_)
// ============== AudioOutputSPDIF3 ====================
class OSCAudioOutputSPDIF3 : public AudioOutputSPDIF3, public OSCAudioBase
{
    public:
        OSCAudioOutputSPDIF3(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioOutputSPDIF3(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/m*",";")) {mute_PCM(msg.getBoolean(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // static void mute_PCM(const bool mute);
            // NOT DEFINED: else if (isTarget(msg,addrOff,"/p*",NULL)) {addReplyResult(msg,addrOff,reply,pll_locked(),nameOfTarget); } // static bool pll_locked(void);
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_output_SPDIF3_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_output_SPDIF3_h_(a)
#endif // defined(output_SPDIF3_h_)

#if defined(output_tdm_h_)
// ============== AudioOutputTDM ====================
class OSCAudioOutputTDM : public AudioOutputTDM, public OSCAudioBase
{
    public:
        OSCAudioOutputTDM(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioOutputTDM(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_output_tdm_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_output_tdm_h_(a)
#endif // defined(output_tdm_h_)

#if defined(output_tdm2_h_)
// ============== AudioOutputTDM2 ====================
class OSCAudioOutputTDM2 : public AudioOutputTDM2, public OSCAudioBase
{
    public:
        OSCAudioOutputTDM2(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioOutputTDM2(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_output_tdm2_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_output_tdm2_h_(a)
#endif // defined(output_tdm2_h_)

// ============== AudioOutputUSB ====================
class OSCAudioOutputUSB : public AudioOutputUSB, public OSCAudioBase
{
    public:
        OSCAudioOutputUSB(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioOutputUSB(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};

#if defined(play_memory_h_)
// ============== AudioPlayMemory ====================
class OSCAudioPlayMemory : public AudioPlayMemory, public OSCAudioBase
{
    public:
        OSCAudioPlayMemory(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioPlayMemory(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/i*",NULL)) {addReplyResult(msg,addrOff,reply,isPlaying(),nameOfTarget); } // bool isPlaying(void) { return playing; }
            else if (isTarget(msg,addrOff,"/l*",NULL)) {addReplyResult(msg,addrOff,reply,lengthMillis(),nameOfTarget); } // uint32_t lengthMillis(void);
            else if (isTarget(msg,addrOff,"/pl*","i")) {play(msg); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void play(const unsigned int *data);
            else if (isTarget(msg,addrOff,"/po*",NULL)) {addReplyResult(msg,addrOff,reply,positionMillis(),nameOfTarget); } // uint32_t positionMillis(void);
            else if (isTarget(msg,addrOff,"/s*",NULL)) {stop(); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void stop(void);
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
	private:
		void play(OSCMessage& msg);
};
#define OSC_CLASS_play_memory_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_play_memory_h_(a)
#endif // defined(play_memory_h_)

#if defined(play_queue_h_)
// ============== AudioPlayQueue ====================
class OSCAudioPlayQueue : public AudioPlayQueue, public OSCAudioBase
{
    public:
        OSCAudioPlayQueue(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioPlayQueue(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/a*",NULL)) {addReplyResult(msg,addrOff,reply,available(),nameOfTarget); } // bool available(void);
            else if (isTarget(msg,addrOff,"/g*",NULL)) {addReplyResult(msg,addrOff,reply,(uint32_t)getBuffer(),nameOfTarget); } // int16_t * getBuffer(void);
            else if (isTarget(msg,addrOff,"/playB*",NULL)) {addReplyResult(msg,addrOff,reply,playBuffer(),nameOfTarget); } // uint32_t playBuffer(void);
            // else if (isTarget(msg,addrOff,"/play","bi")) {addReplyResult(msg,addrOff,reply,play(msg.getBlob(0),msg.getInt(1)),nameOfTarget); } // uint32_t play(const int16_t *data, uint32_t len);
            else if (isTarget(msg,addrOff,"/play","i")) {addReplyResult(msg,addrOff,reply,play(msg.getInt(0)),nameOfTarget); } // uint32_t play(int16_t data);
            else if (isTarget(msg,addrOff,"/setB*","i")) {setBehaviour((behaviour_e) msg.getInt(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void setBehaviour(behaviour_e behave) {behaviour = behave;}
            else if (isTarget(msg,addrOff,"/setM*","i")) {setMaxBuffers(msg.getInt(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void setMaxBuffers(uint8_t);
            // NOT DEFINED: else if (isTarget(msg,addrOff,"/st*",NULL)) {stop(); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void stop(void);
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_play_queue_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_play_queue_h_(a)
#endif // defined(play_queue_h_)

#if defined(play_sd_raw_h_)
// ============== AudioPlaySdRaw ====================
class OSCAudioPlaySdRaw : public AudioPlaySdRaw, public OSCAudioBase
{
    public:
        OSCAudioPlaySdRaw(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioPlaySdRaw(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/b*",NULL)) {begin(); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void begin(void);
            else if (isTarget(msg,addrOff,"/i*",NULL)) {addReplyResult(msg,addrOff,reply,isPlaying(),nameOfTarget); } // bool isPlaying(void) { return playing; }
            else if (isTarget(msg,addrOff,"/l*",NULL)) {addReplyResult(msg,addrOff,reply,lengthMillis(),nameOfTarget); } // uint32_t lengthMillis(void);
            else if (isTarget(msg,addrOff,"/pl*","s")) {play(msg); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // bool play(const char *filename);
            else if (isTarget(msg,addrOff,"/po*",NULL)) {addReplyResult(msg,addrOff,reply,positionMillis(),nameOfTarget); } // uint32_t positionMillis(void);
            else if (isTarget(msg,addrOff,"/s*",NULL)) {stop(); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void stop(void);
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
	private:
		void play(OSCMessage& msg);
};
#define OSC_CLASS_play_sd_raw_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_play_sd_raw_h_(a)
#endif // defined(play_sd_raw_h_)

#if defined(play_sd_wav_h_)
// ============== AudioPlaySdWav ====================
class OSCAudioPlaySdWav : public AudioPlaySdWav, public OSCAudioBase
{
    public:
        OSCAudioPlaySdWav(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioPlaySdWav(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/b*",NULL)) {begin(); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void begin(void);
            else if (isTarget(msg,addrOff,"/isPa*",NULL)) {addReplyResult(msg,addrOff,reply,isPaused(),nameOfTarget); } // bool isPaused(void);
            else if (isTarget(msg,addrOff,"/isPl*",NULL)) {addReplyResult(msg,addrOff,reply,isPlaying(),nameOfTarget); } // bool isPlaying(void);
            else if (isTarget(msg,addrOff,"/isS*",NULL)) {addReplyResult(msg,addrOff,reply,isStopped(),nameOfTarget); } // bool isStopped(void);
            else if (isTarget(msg,addrOff,"/l*",NULL)) {addReplyResult(msg,addrOff,reply,lengthMillis(),nameOfTarget); } // uint32_t lengthMillis(void);
            else if (isTarget(msg,addrOff,"/pl*","s")) {play(msg); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // bool play(const char *filename);
            else if (isTarget(msg,addrOff,"/po*",NULL)) {addReplyResult(msg,addrOff,reply,positionMillis(),nameOfTarget); } // uint32_t positionMillis(void);
            else if (isTarget(msg,addrOff,"/s*",NULL)) {stop(); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void stop(void);
            else if (isTarget(msg,addrOff,"/t*",NULL)) {togglePlayPause(); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void togglePlayPause(void);
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
	private:
		void play(OSCMessage& msg);
};
#define OSC_CLASS_play_sd_wav_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_play_sd_wav_h_(a)
#endif // defined(play_sd_wav_h_)

#if defined(play_serial_raw_h_)
// ============== AudioPlaySerialflashRaw ====================
class OSCAudioPlaySerialflashRaw : public AudioPlaySerialflashRaw, public OSCAudioBase
{
    public:
        OSCAudioPlaySerialflashRaw(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioPlaySerialflashRaw(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/b*",NULL)) {begin(); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void begin(void);
            else if (isTarget(msg,addrOff,"/i*",NULL)) {addReplyResult(msg,addrOff,reply,isPlaying(),nameOfTarget); } // bool isPlaying(void) { return playing; }
            else if (isTarget(msg,addrOff,"/l*",NULL)) {addReplyResult(msg,addrOff,reply,lengthMillis(),nameOfTarget); } // uint32_t lengthMillis(void);
            else if (isTarget(msg,addrOff,"/pl*","s")) {play(msg); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // bool play(const char *filename);
            else if (isTarget(msg,addrOff,"/po*",NULL)) {addReplyResult(msg,addrOff,reply,positionMillis(),nameOfTarget); } // uint32_t positionMillis(void);
            else if (isTarget(msg,addrOff,"/s*",NULL)) {stop(); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void stop(void);
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
	private:
		void play(OSCMessage& msg);
};
#define OSC_CLASS_play_serial_raw_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_play_serial_raw_h_(a)
#endif // defined(play_serial_raw_h_)

#if defined(record_queue_h_)
// ============== AudioRecordQueue ====================
class OSCAudioRecordQueue : public AudioRecordQueue, public OSCAudioBase
{
    public:
        OSCAudioRecordQueue(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioRecordQueue(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/a*",NULL)) {addReplyResult(msg,addrOff,reply,(int32_t)available(),nameOfTarget); } // int available(void);
            else if (isTarget(msg,addrOff,"/b*",NULL)) {begin(); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void begin(void) {
            else if (isTarget(msg,addrOff,"/c*",NULL)) {clear(); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void clear(void);
            else if (isTarget(msg,addrOff,"/e*",NULL)) {end(); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void end(void) {
            else if (isTarget(msg,addrOff,"/f*",NULL)) {freeBuffer(); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void freeBuffer(void);
            else if (isTarget(msg,addrOff,"/r*",NULL)) {addReplyResult(msg,addrOff,reply,(uint32_t)readBuffer(),nameOfTarget); } // int16_t * readBuffer(void);
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_record_queue_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_record_queue_h_(a)
#endif // defined(record_queue_h_)

#if defined(synth_karplusstrong_h_)
// ============== AudioSynthKarplusStrong ====================
class OSCAudioSynthKarplusStrong : public AudioSynthKarplusStrong, public OSCAudioBase
{
    public:
        OSCAudioSynthKarplusStrong(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioSynthKarplusStrong(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/noteOf*","f")) {noteOff(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void noteOff(float velocity) {
            else if (isTarget(msg,addrOff,"/noteOn","ff")) {noteOn(msg.getFloat(0),msg.getFloat(1)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void noteOn(float frequency, float velocity) {
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_synth_karplusstrong_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_synth_karplusstrong_h_(a)
#endif // defined(synth_karplusstrong_h_)

#if defined(synth_pinknoise_h_)
// ============== AudioSynthNoisePink ====================
class OSCAudioSynthNoisePink : public AudioSynthNoisePink, public OSCAudioBase
{
    public:
        OSCAudioSynthNoisePink(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioSynthNoisePink(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/a*","f")) {amplitude(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void amplitude(float n) {
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_synth_pinknoise_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_synth_pinknoise_h_(a)
#endif // defined(synth_pinknoise_h_)

#if defined(synth_whitenoise_h_)
// ============== AudioSynthNoiseWhite ====================
class OSCAudioSynthNoiseWhite : public AudioSynthNoiseWhite, public OSCAudioBase
{
    public:
        OSCAudioSynthNoiseWhite(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioSynthNoiseWhite(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/a*","f")) {amplitude(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void amplitude(float n) {
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_synth_whitenoise_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_synth_whitenoise_h_(a)
#endif // defined(synth_whitenoise_h_)

#if defined(_SYNTH_SIMPLE_DRUM_H_)
// ============== AudioSynthSimpleDrum ====================
class OSCAudioSynthSimpleDrum : public AudioSynthSimpleDrum, public OSCAudioBase
{
    public:
        OSCAudioSynthSimpleDrum(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioSynthSimpleDrum(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/f*","f")) {frequency(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void frequency(float freq)
            else if (isTarget(msg,addrOff,"/l*","i")) {length(msg.getInt(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void length(int32_t milliseconds)
            else if (isTarget(msg,addrOff,"/n*",NULL)) {noteOn(); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void noteOn();
            else if (isTarget(msg,addrOff,"/p*","f")) {pitchMod(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void pitchMod(float depth);
            else if (isTarget(msg,addrOff,"/s*","f")) {secondMix(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void secondMix(float level);
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS__SYNTH_SIMPLE_DRUM_H_(a) OSC_CLASS(a)
#else
#define OSC_CLASS__SYNTH_SIMPLE_DRUM_H_(a)
#endif // defined(_SYNTH_SIMPLE_DRUM_H_)

#if defined(synth_tonesweep_h_)
// ============== AudioSynthToneSweep ====================
class OSCAudioSynthToneSweep : public AudioSynthToneSweep, public OSCAudioBase
{
    public:
        OSCAudioSynthToneSweep(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioSynthToneSweep(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/i*",NULL)) {addReplyResult(msg,addrOff,reply,(uint8_t)isPlaying(),nameOfTarget); } // unsigned char isPlaying(void);
            else if (isTarget(msg,addrOff,"/p*","fiif")) {addReplyResult(msg,addrOff,reply,(bool)play(msg.getFloat(0),msg.getInt(1),msg.getInt(2),msg.getFloat(3)),nameOfTarget); } // boolean play(float t_amp,int t_lo,int t_hi,float t_time);
            else if (isTarget(msg,addrOff,"/r*",NULL)) {addReplyResult(msg,addrOff,reply,read(),nameOfTarget); } // float read(void) {
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_synth_tonesweep_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_synth_tonesweep_h_(a)
#endif // defined(synth_tonesweep_h_)

#if defined(synth_waveform_h_)
// ============== AudioSynthWaveform ====================
class OSCAudioSynthWaveform : public AudioSynthWaveform, public OSCAudioBase
{
    public:
        OSCAudioSynthWaveform(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this), arbdata(0) {}
        OSCAudioSynthWaveform(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this), arbdata(0) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/am*","f")) {amplitude(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void amplitude(float n) {	// 0 to 1.0
            else if (isTarget(msg,addrOff,"/ar*","bf")) {addReplyResult(msg,addrOff,reply,arbitraryWaveform(msg),nameOfTarget); } // void arbitraryWaveform(const int16_t *data, float maxFreq) {
            else if (isTarget(msg,addrOff,"/b*","ffi")) {begin(msg.getFloat(0),msg.getFloat(1),msg.getInt(2)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void begin(float t_amp, float t_freq, short t_type) {
            else if (isTarget(msg,addrOff,"/b*","i")) {begin(msg.getInt(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void begin(short t_type) {
            else if (isTarget(msg,addrOff,"/f*","f")) {frequency(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void frequency(float freq) {
            else if (isTarget(msg,addrOff,"/o*","f")) {offset(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void offset(float n) {
            else if (isTarget(msg,addrOff,"/ph*","f")) {phase(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void phase(float angle) {
            else if (isTarget(msg,addrOff,"/pu*","f")) {pulseWidth(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void pulseWidth(float n) {	// 0.0 to 1.0
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
	private:
		bool arbitraryWaveform(OSCMessage& msg);
		int16_t* arbdata;
};
#define OSC_CLASS_synth_waveform_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_synth_waveform_h_(a)
#endif // defined(synth_waveform_h_)

#if defined(synth_dc_h_)
// ============== AudioSynthWaveformDc ====================
class OSCAudioSynthWaveformDc : public AudioSynthWaveformDc, public OSCAudioBase
{
    public:
        OSCAudioSynthWaveformDc(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioSynthWaveformDc(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/a*","f")) {amplitude(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void amplitude(float n) {
            else if (isTarget(msg,addrOff,"/a*","ff")) {amplitude(msg.getFloat(0),msg.getFloat(1)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void amplitude(float n, float milliseconds) {
            else if (isTarget(msg,addrOff,"/r*",NULL)) {addReplyResult(msg,addrOff,reply,read(),nameOfTarget); } // float read(void) {
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_synth_dc_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_synth_dc_h_(a)
#endif // defined(synth_dc_h_)

#if defined(synth_waveform_h_)
// ============== AudioSynthWaveformModulated ====================
class OSCAudioSynthWaveformModulated : public AudioSynthWaveformModulated, public OSCAudioBase
{
    public:
        OSCAudioSynthWaveformModulated(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this), arbdata(0) {}
        OSCAudioSynthWaveformModulated(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this), arbdata(0) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/am*","f")) {amplitude(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void amplitude(float n) {	// 0 to 1.0
            else if (isTarget(msg,addrOff,"/ar*","bf")) {addReplyResult(msg,addrOff,reply,arbitraryWaveform(msg),nameOfTarget); } // void arbitraryWaveform(const int16_t *data, float maxFreq) {
            else if (isTarget(msg,addrOff,"/b*","ffi")) {begin(msg.getFloat(0),msg.getFloat(1),msg.getInt(2)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void begin(float t_amp, float t_freq, short t_type) {
            else if (isTarget(msg,addrOff,"/b*","i")) {begin(msg.getInt(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void begin(short t_type) {
            else if (isTarget(msg,addrOff,"/frequencyM*","f")) {frequencyModulation(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void frequencyModulation(float octaves) {
            else if (isTarget(msg,addrOff,"/frequency","f")) {frequency(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void frequency(float freq) {
            else if (isTarget(msg,addrOff,"/o*","f")) {offset(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void offset(float n) {
            else if (isTarget(msg,addrOff,"/p*","f")) {phaseModulation(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void phaseModulation(float degrees) {
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
	private:
		bool arbitraryWaveform(OSCMessage& msg);
		int16_t* arbdata;
};
#define OSC_CLASS_synth_waveform_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_synth_waveform_h_(a)
#endif // defined(synth_waveform_h_)

#if defined(synth_pwm_h_)
// ============== AudioSynthWaveformPWM ====================
class OSCAudioSynthWaveformPWM : public AudioSynthWaveformPWM, public OSCAudioBase
{
    public:
        OSCAudioSynthWaveformPWM(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioSynthWaveformPWM(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/a*","f")) {amplitude(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void amplitude(float n) {
            else if (isTarget(msg,addrOff,"/f*","f")) {frequency(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void frequency(float freq) {
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_synth_pwm_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_synth_pwm_h_(a)
#endif // defined(synth_pwm_h_)

#if defined(synth_sine_h_)
// ============== AudioSynthWaveformSine ====================
class OSCAudioSynthWaveformSine : public AudioSynthWaveformSine, public OSCAudioBase
{
    public:
        OSCAudioSynthWaveformSine(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioSynthWaveformSine(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/a*","f")) {amplitude(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void amplitude(float n) {
            else if (isTarget(msg,addrOff,"/f*","f")) {frequency(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void frequency(float freq) {
            else if (isTarget(msg,addrOff,"/p*","f")) {phase(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void phase(float angle) {
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_synth_sine_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_synth_sine_h_(a)
#endif // defined(synth_sine_h_)

#if defined(synth_sine_h_)
// ============== AudioSynthWaveformSineHires ====================
class OSCAudioSynthWaveformSineHires : public AudioSynthWaveformSineHires, public OSCAudioBase
{
    public:
        OSCAudioSynthWaveformSineHires(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioSynthWaveformSineHires(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/a*","f")) {amplitude(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void amplitude(float n) {
            else if (isTarget(msg,addrOff,"/f*","f")) {frequency(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void frequency(float freq) {
            else if (isTarget(msg,addrOff,"/p*","f")) {phase(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void phase(float angle) {
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_synth_sine_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_synth_sine_h_(a)
#endif // defined(synth_sine_h_)

#if defined(synth_sine_h_)
// ============== AudioSynthWaveformSineModulated ====================
class OSCAudioSynthWaveformSineModulated : public AudioSynthWaveformSineModulated, public OSCAudioBase
{
    public:
        OSCAudioSynthWaveformSineModulated(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioSynthWaveformSineModulated(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/a*","f")) {amplitude(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void amplitude(float n) {
            else if (isTarget(msg,addrOff,"/f*","f")) {frequency(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void frequency(float freq) {
            else if (isTarget(msg,addrOff,"/p*","f")) {phase(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void phase(float angle) {
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};
#define OSC_CLASS_synth_sine_h_(a) OSC_CLASS(a)
#else
#define OSC_CLASS_synth_sine_h_(a)
#endif // defined(synth_sine_h_)

// ============== AudioSynthWavetable ====================
class OSCAudioSynthWavetable : public AudioSynthWavetable, public OSCAudioBase
{
    public:
        OSCAudioSynthWavetable(const char* _name) :  OSCAudioBase(_name, (AudioStream*) this) {}
        OSCAudioSynthWavetable(const char* _name, OSCAudioGroup& grp) :  OSCAudioBase(_name, grp, (AudioStream*) this) {}

        void route(OSCMessage& msg, int addrOff, OSCBundle& reply)
        {
          int nameOff;
          if ((nameOff = isMine(msg,addrOff)) > 0)
          {
            addrOff += nameOff;
            char* nameOfTarget;
            nameOfTarget = alloca(getPathNameLength(this)+1);
            if (NULL != nameOfTarget)
              getPathNameTo(this,nameOfTarget);
            if (isTarget(msg,addrOff,"/a*","f")) {amplitude(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void amplitude(float v) {
            else if (isTarget(msg,addrOff,"/f*","f")) {addReplyResult(msg,addrOff,reply,(int32_t)freqToNote(msg.getFloat(0)),nameOfTarget); } // static int freqToNote(float freq) {
            else if (isTarget(msg,addrOff,"/g*",NULL)) {addReplyResult(msg,addrOff,reply,(uint8_t)getEnvState(),nameOfTarget); } // envelopeStateEnum getEnvState(void) { return env_state; }
            else if (isTarget(msg,addrOff,"/i*",NULL)) {addReplyResult(msg,addrOff,reply,isPlaying(),nameOfTarget); } // bool isPlaying(void) { return env_state != STATE_IDLE; }
            else if (isTarget(msg,addrOff,"/m*","i")) {addReplyResult(msg,addrOff,reply,midi_volume_transform(msg.getInt(0)),nameOfTarget); } // static float midi_volume_transform(int midi_amp) {
            else if (isTarget(msg,addrOff,"/n*","i")) {addReplyResult(msg,addrOff,reply,noteToFreq(msg.getInt(0)),nameOfTarget); } // static float noteToFreq(int note) {
            else if (isTarget(msg,addrOff,"/playF*","fi")) {playFrequency(msg.getFloat(0),msg.getInt(1)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void playFrequency(float freq, int amp = DEFAULT_AMPLITUDE);
            else if (isTarget(msg,addrOff,"/playN*","ii")) {playNote(msg.getInt(0),msg.getInt(1)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void playNote(int note, int amp = DEFAULT_AMPLITUDE);
            else if (isTarget(msg,addrOff,"/setF*","f")) {setFrequency(msg.getFloat(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void setFrequency(float freq);
            // else if (isTarget(msg,addrOff,"/setI*","b")) {setInstrument(msg.getBlob(0)); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void setInstrument(const instrument_data& instrument) {
            else if (isTarget(msg,addrOff,"/st*",NULL)) {stop(); addReplyExecuted(msg,addrOff,reply,nameOfTarget);} // void stop(void);
            else addReplyResult(msg,addrOff,reply,false,nameOfTarget,INVALID_METHOD);
          }
		}
};

#define OSC_AUDIO_CLASSES \
	OSC_CLASS_async_input_spdif3_h_(AsyncAudioInputSPDIF3) \
	OSC_CLASS_mixer_h_(AudioAmplifier) \
	OSC_CLASS_analyze_fft1024_h_(AudioAnalyzeFFT1024) \
	OSC_CLASS_analyze_fft256_h_(AudioAnalyzeFFT256) \
	OSC_CLASS_AudioAnalyzeNoteFrequency_h_(AudioAnalyzeNoteFrequency) \
	OSC_CLASS_analyze_peakdetect_h_(AudioAnalyzePeak) \
	OSC_CLASS_analyze_print_h_(AudioAnalyzePrint) \
	OSC_CLASS_analyze_rms_h_(AudioAnalyzeRMS) \
	OSC_CLASS_analyze_tonedetect_h_(AudioAnalyzeToneDetect) \
	OSC_CLASS_control_ak4558_h_(AudioControlAK4558) \
	OSC_CLASS_control_cs42448_h_(AudioControlCS42448) \
	OSC_CLASS_control_cs4272_h_(AudioControlCS4272) \
	OSC_CLASS_control_sgtl5000_h_(AudioControlSGTL5000) \
	OSC_CLASS_control_tlv320aic3206_h_(AudioControlTLV320AIC3206) \
	OSC_CLASS_control_wm8731_h_(AudioControlWM8731) \
	OSC_CLASS_control_wm8731_h_(AudioControlWM8731master) \
	OSC_CLASS_effect_bitcrusher_h_(AudioEffectBitcrusher) \
	OSC_CLASS_effect_delay_h_(AudioEffectDelay) \
	OSC_CLASS_effect_delay_ext_h_(AudioEffectDelayExternal) \
	OSC_CLASS_effect_digital_combine_h_(AudioEffectDigitalCombine) \
	OSC_CLASS_effect_envelope_h_(AudioEffectEnvelope) \
	OSC_CLASS_effect_fade_h_(AudioEffectFade) \
	OSC_CLASS_effect_freeverb_h_(AudioEffectFreeverb) \
	OSC_CLASS_effect_freeverb_h_(AudioEffectFreeverbStereo) \
	OSC_CLASS(AudioEffectGranular) \
	OSC_CLASS_effect_midside_decode_h_(AudioEffectMidSide) \
	OSC_CLASS_effect_multiply_h_(AudioEffectMultiply) \
	OSC_CLASS_effect_rectifier_h_(AudioEffectRectifier) \
	OSC_CLASS_effect_reverb_(AudioEffectReverb) \
	OSC_CLASS_effect_wavefolder_h_(AudioEffectWaveFolder) \
	OSC_CLASS_effect_waveshaper_h_(AudioEffectWaveshaper) \
	OSC_CLASS_filter_biquad_h_(AudioFilterBiquad) \
	OSC_CLASS_filter_fir_h_(AudioFilterFIR) \
	OSC_CLASS_filter_ladder_h_(AudioFilterLadder) \
	OSC_CLASS_filter_variable_h_(AudioFilterStateVariable) \
	OSC_CLASS_input_adc_h_(AudioInputAnalog) \
	OSC_CLASS_input_adcs_h_(AudioInputAnalogStereo) \
	OSC_CLASS__input_i2s_h_(AudioInputI2S) \
	OSC_CLASS__input_i2s2_h_(AudioInputI2S2) \
	OSC_CLASS__input_i2s_hex_h_(AudioInputI2SHex) \
	OSC_CLASS__input_i2s_oct_h_(AudioInputI2SOct) \
	OSC_CLASS__input_i2s_quad_h_(AudioInputI2SQuad) \
	OSC_CLASS__input_pdm_h_(AudioInputPDM) \
	OSC_CLASS__input_pdm_i2s2_h_(AudioInputPDM2) \
	OSC_CLASS__input_spdif3_h_(AudioInputSPDIF3) \
	OSC_CLASS__input_tdm_h_(AudioInputTDM) \
	OSC_CLASS__input_tdm2_h_(AudioInputTDM2) \
	OSC_CLASS(AudioInputUSB) \
	OSC_CLASS_mixer_h_(AudioMixer4) \
	OSC_CLASS_output_ADAT_h_(AudioOutputADAT) \
	OSC_CLASS_output_dac_h_(AudioOutputAnalog) \
	OSC_CLASS_output_dacs_h_(AudioOutputAnalogStereo) \
	OSC_CLASS_output_i2s_h_(AudioOutputI2S) \
	OSC_CLASS_output_i2s2_h_(AudioOutputI2S2) \
	OSC_CLASS_output_i2s_hex_h_(AudioOutputI2SHex) \
	OSC_CLASS_output_i2s_oct_h_(AudioOutputI2SOct) \
	OSC_CLASS_output_i2s_quad_h_(AudioOutputI2SQuad) \
	OSC_CLASS_output_mqs_h_(AudioOutputMQS) \
	OSC_CLASS_output_pt8211_h_(AudioOutputPT8211) \
	OSC_CLASS_output_pt8211_2_h_(AudioOutputPT8211_2) \
	OSC_CLASS_output_pwm_h_(AudioOutputPWM) \
	OSC_CLASS_output_SPDIF_h_(AudioOutputSPDIF) \
	OSC_CLASS_output_SPDIF2_h_(AudioOutputSPDIF2) \
	OSC_CLASS_output_SPDIF3_h_(AudioOutputSPDIF3) \
	OSC_CLASS_output_tdm_h_(AudioOutputTDM) \
	OSC_CLASS_output_tdm2_h_(AudioOutputTDM2) \
	OSC_CLASS(AudioOutputUSB) \
	OSC_CLASS_play_memory_h_(AudioPlayMemory) \
	OSC_CLASS_play_queue_h_(AudioPlayQueue) \
	OSC_CLASS_play_sd_raw_h_(AudioPlaySdRaw) \
	OSC_CLASS_play_sd_wav_h_(AudioPlaySdWav) \
	OSC_CLASS_play_serial_raw_h_(AudioPlaySerialflashRaw) \
	OSC_CLASS_record_queue_h_(AudioRecordQueue) \
	OSC_CLASS_synth_karplusstrong_h_(AudioSynthKarplusStrong) \
	OSC_CLASS_synth_pinknoise_h_(AudioSynthNoisePink) \
	OSC_CLASS_synth_whitenoise_h_(AudioSynthNoiseWhite) \
	OSC_CLASS__SYNTH_SIMPLE_DRUM_H_(AudioSynthSimpleDrum) \
	OSC_CLASS_synth_tonesweep_h_(AudioSynthToneSweep) \
	OSC_CLASS_synth_waveform_h_(AudioSynthWaveform) \
	OSC_CLASS_synth_dc_h_(AudioSynthWaveformDc) \
	OSC_CLASS_synth_waveform_h_(AudioSynthWaveformModulated) \
	OSC_CLASS_synth_pwm_h_(AudioSynthWaveformPWM) \
	OSC_CLASS_synth_sine_h_(AudioSynthWaveformSine) \
	OSC_CLASS_synth_sine_h_(AudioSynthWaveformSineHires) \
	OSC_CLASS_synth_sine_h_(AudioSynthWaveformSineModulated) \
	OSC_CLASS(AudioSynthWavetable) \


#endif // defined(OSC_AUTOGEN_H)
