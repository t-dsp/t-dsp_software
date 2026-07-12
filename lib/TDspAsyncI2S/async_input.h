/* Audio Library for Teensy 3.X
 * Copyright (c) 2019, Paul Stoffregen, paul@pjrc.com
 *
 * Development of this audio library was funded by PJRC.COM, LLC by sales of
 * Teensy and Audio Adaptor boards.  Please support PJRC's efforts to develop
 * open source software by purchasing Teensy or other PJRC products.
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
 by Alexander Walch
*/
/*
 * T-DSP note: this is Alexander Walch's async-input resampler wrapper from
 * JayShoe/esp32_T4_bt_music_receiver (assembled with alex6679), used to receive
 * the ESP32's I2S Bluetooth audio (a free-running master clock) into the Teensy
 * audio graph (a separate crystal) WITHOUT clicks, by async-resampling the
 * incoming stream to AUDIO_SAMPLE_RATE. See project memory
 * project_esp32_i2s_audio_spike / project_spdif_optical_ram_fit.
 *
 * The ONLY change vs the reference is porting three Resampler calls from the
 * old API to alex6679's CURRENT Resampler (the one already vendored in
 * lib/Audio, per UPSTREAM_OPENAUDIO_PLAN.md):
 *     addToSampleDiff(diff) -> updateIncrement(diff)   (same: feed the PI loop, returns 'settled')
 *     addToPos(frac)        -> setPos(frac)            (frac is the absolute [0,1) sub-sample pos here)
 *     fixStep()             -> fixIncrement()          (same: freeze the adapted step)
 * and widening the resample() length locals to int32_t to match the current
 * Resampler::resample<N>() signature (int32_t& processedLength / outputCount).
 * This mirrors how the in-repo async_input_spdif3.cpp drives the same Resampler.
 */
#ifndef async_input_h_
#define async_input_h_
#include "Resampler.h"
#include "Quantizer.h"
#include "biquad.h"
#include "FrequencyMeasurement.h"
#include "AudioStream.h"
#include "Audio.h"
#include "DMAChannel.h"
#include "Arduino.h"
#include <arm_math.h>


template <typename TInput>
class AsyncAudioInput : public AudioStream
{
public:

  #define NOCHANNELS TInput::getNumberOfChannels()

  ///@param attenuation target attenuation [dB] of the anti-aliasing filter. Only used if newFs<fs. The attenuation can't be reached if the needed filter length exceeds 2*MAX_FILTER_SAMPLES+1
  ///@param minHalfFilterLength If newFs >= fs, the filter length of the resampling filter is 2*minHalfFilterLength+1. If fs y newFs the filter is maybe longer to reach the desired attenuation
  AsyncAudioInput(bool dither=false, bool noiseshaping=false,float attenuation=100, int32_t minHalfFilterLength=20, int32_t maxHalfFilterLength= 80):
    AudioStream(0, NULL),
    _resampler(attenuation, minHalfFilterLength, maxHalfFilterLength)
    {
    _inputFrequency=-1.;
    const float factor = powf(2, 15)-1.f; // to 16 bit audio

    _bufferLPFilter.pCoeffs=new float[5];
    _bufferLPFilter.numStages=1;
    _bufferLPFilter.pState=new float[2];
    getCoefficients(_bufferLPFilter.pCoeffs, BiquadType::LOW_PASS, 0., 5., AUDIO_SAMPLE_RATE_EXACT/AUDIO_BLOCK_SAMPLES, 0.5);
    float* b[NOCHANNELS];
    for (uint8_t i =0 ; i< NOCHANNELS; i++){
      b[i]=&(_buffer[i][0]);
      quantizer[i]= new Quantizer(AUDIO_SAMPLE_RATE);
      quantizer[i]->configure(noiseshaping, dither, factor);
    }
    _noSamplesPerIsr=TInput::getNumberOfSamplesPerIsr();
    _resampleOffset=0;
    __disable_irq();
    _input.setResampleBuffer(b, bufferLength);
    _input.setResampleOffset(_resampleOffset);
    _input.setFrequencyMeasurment(updateFrequMeasure);
    __enable_irq();
  }
  ~AsyncAudioInput(){
    delete [] _bufferLPFilter.pCoeffs;
    delete [] _bufferLPFilter.pState;
    for (uint8_t i =0 ; i< NOCHANNELS; i++){
      delete quantizer[i];
    }
  }

  virtual void update(void)
  {
    frequMeasure.computeFrequency();
    configure();
    monitorResampleBuffer();  //important first call 'monitorResampleBuffer' then 'resample'
    audio_block_t *blocks[NOCHANNELS];
    audio_block_t** b=blocks;
    for (uint8_t i =0; i< NOCHANNELS; i++){
      *b= allocate();
      if(*b==nullptr){
        b=blocks;
        for (uint8_t j=0; j< i; j++){
          release(*b);
          *b++=nullptr;
        }
        break;
      }
      ++b;
    }
    if (blocks[0]) {
      int32_t block_offset=0;
      resample(blocks, block_offset);
      if(block_offset < AUDIO_BLOCK_SAMPLES){
        for (uint8_t i =0; i< NOCHANNELS; i++){
          memset(blocks[i]->data+block_offset, 0, (AUDIO_BLOCK_SAMPLES-block_offset)*sizeof(int16_t));
        }
      }
      for (uint8_t i =0; i< NOCHANNELS; i++){
        transmit(blocks[i], i);
        release(blocks[i]);
        blocks[i]=nullptr;
      }
    }
  }

  double getTargetLantency() const {
    AudioNoInterrupts();
    double l=_targetLatencyS;
    AudioInterrupts();
    return l ;
  }

  double getAttenuation() const{
    AudioNoInterrupts();
    double a= _resampler.getAttenuation();
    AudioInterrupts();
    return a;
  }

  int32_t getHalfFilterLength() const{
    AudioNoInterrupts();
    int32_t l= _resampler.getHalfFilterLength();
    AudioInterrupts();
    return l;
  }

  double getInputFrequency() const {
    AudioNoInterrupts();
    double f=frequMeasure.getFrequency();
    AudioInterrupts();
    return f;
  }

  double getBufferedTime() const{
    AudioNoInterrupts();
    double n=_bufferedTime;
    AudioInterrupts();
    return n;
  }
private:

  static FrequencyMeasurement frequMeasure;
  Resampler _resampler;
  Quantizer* quantizer[NOCHANNELS];
  TInput _input;
  arm_biquad_cascade_df2T_instance_f32 _bufferLPFilter;
  constexpr static double blockDuration=AUDIO_BLOCK_SAMPLES/AUDIO_SAMPLE_RATE; //[seconds]
  double _maxLatency=2.*blockDuration;
  constexpr static int32_t bufferLength=(int32_t)( 1.f + ceilf(192000.f/AUDIO_SAMPLE_RATE))*AUDIO_BLOCK_SAMPLES + (int32_t)(ceilf(3.f*blockDuration * 192000.f)); //maximum input sample rate is 192kHz
  float _buffer[NOCHANNELS][bufferLength];
  volatile double _bufferedTime;
  double _inputFrequency;
  int32_t _noSamplesPerIsr;
  int32_t _resampleOffset;
  double _targetLatencyS; //target latency [seconds]


  void resample(audio_block_t **blocks, int32_t& block_offset){
    block_offset=0;
    if(!_resampler.initialized()){
      return;
    }
    __disable_irq();
    int32_t bOffset=_input.getBufferOffset();
    __enable_irq();

    int32_t inputBufferStop = bOffset >= _resampleOffset ? bOffset-_resampleOffset : bufferLength-_resampleOffset;
    if (inputBufferStop==0){
      return;
    }

    int32_t processedLength=0;
    int32_t outputCount=0;
    int32_t outputLength=AUDIO_BLOCK_SAMPLES;
    float outputs[NOCHANNELS][AUDIO_BLOCK_SAMPLES];
    float* outputsPtr[NOCHANNELS];
    float* inputs[NOCHANNELS];
    for (uint8_t i =0 ; i< NOCHANNELS; i++){
      inputs[i]=_buffer[i]+_resampleOffset;
      outputsPtr[i]=&(outputs[i][0]);
    }
    _resampler.resample<NOCHANNELS>(inputs, inputBufferStop, processedLength, outputsPtr, outputLength, outputCount);

    _resampleOffset=(_resampleOffset+processedLength)%bufferLength;
    block_offset=outputCount;

    if (bOffset > _resampleOffset && block_offset< AUDIO_BLOCK_SAMPLES){
      inputBufferStop= bOffset-_resampleOffset;
      outputLength=AUDIO_BLOCK_SAMPLES-block_offset;
      for (uint8_t i =0 ; i< NOCHANNELS; i++){
        inputs[i]=_buffer[i]+_resampleOffset;
      }
      _resampler.resample<NOCHANNELS>(inputs, inputBufferStop, processedLength, outputsPtr, outputLength, outputCount);
      _resampleOffset=(_resampleOffset+processedLength)%bufferLength;
      block_offset+=outputCount;
    }
    for (int32_t i =0; i< NOCHANNELS; i++){
      quantizer[i]->quantize(outputs[i], blocks[i]->data, block_offset);
    }
    __disable_irq();
    _input.setResampleOffset(_resampleOffset);
    __enable_irq();
  }

  static void updateFrequMeasure(){
    frequMeasure.update();
  }

  void configure(){
    const double inputF=frequMeasure.getFrequency();  //returns: -1 ... invalid frequency
    if (inputF > 0.){
      //we got a valid sample frequency
      const double frequDiff=inputF/_inputFrequency-1.;
      if (abs(frequDiff) > 0.01 || !_resampler.initialized()){
        //the new sample frequency differs from the last one -> configure the _resampler again
        _inputFrequency=inputF;
        _targetLatencyS=max(0.001,(_noSamplesPerIsr*3./2./_inputFrequency));
        _maxLatency=max(2.*blockDuration, 2*_noSamplesPerIsr/_inputFrequency);
        const int32_t targetLatency=round(_targetLatencyS*inputF);
        __disable_irq();
        int32_t bOffset=_input.getBufferOffset();
        _resampleOffset =  targetLatency <=  bOffset ? bOffset - targetLatency : bufferLength -(targetLatency-bOffset);
        _input.setResampleOffset(_resampleOffset);
        __enable_irq();
        _resampler.configure(_inputFrequency,AUDIO_SAMPLE_RATE);
      }
    }
  }

  void monitorResampleBuffer(){
    if(!_resampler.initialized()){
      return;
    }

    __disable_irq();

    int32_t bOffset=_input.getBufferOffset();
    const double dmaOffset=(micros()-frequMeasure.getTimeOfLastUpdate())*1e-6;  //[seconds]

    double inputFrequency=frequMeasure.getLastValidFrequency();

    double bTime = _resampleOffset <= bOffset ? (bOffset-_resampleOffset-_resampler.getXPos())/inputFrequency+dmaOffset : (bufferLength-_resampleOffset +bOffset-_resampler.getXPos())/inputFrequency+dmaOffset;  //[seconds]

    double diff = bTime- (blockDuration+ _targetLatencyS);  //seconds

    biquad_cascade_df2T<double, arm_biquad_cascade_df2T_instance_f32, float>(&_bufferLPFilter, &diff, &diff, 1);

    bool settled=_resampler.updateIncrement(diff);

    if (bTime > _maxLatency || bTime-dmaOffset<= blockDuration || settled) {
      double distance=(blockDuration+_targetLatencyS-dmaOffset)*inputFrequency+_resampler.getXPos();
      diff=0.;
      if (distance > bufferLength-_noSamplesPerIsr){
        diff=bufferLength-_noSamplesPerIsr-distance;
        distance=bufferLength-_noSamplesPerIsr;
      }
      if (distance < 0.){
        distance=0.;
        diff=- (blockDuration+ _targetLatencyS);
      }
      double resampleOffsetF=bOffset-distance;
      _resampleOffset=(int32_t)floor(resampleOffsetF);
      _resampler.setPos(resampleOffsetF-_resampleOffset);
      while (_resampleOffset<0){
        _resampleOffset+=bufferLength;
      }
      _input.setResampleOffset(_resampleOffset);
      __enable_irq();

      preload(&_bufferLPFilter, (float)diff);
      _resampler.fixIncrement();
    }
    else {
      __enable_irq();
    }
    _bufferedTime=_targetLatencyS+diff;
  }
};
template<typename TInput>
FrequencyMeasurement AsyncAudioInput<TInput>::frequMeasure(TInput::getNumberOfSamplesPerIsr());


#endif
