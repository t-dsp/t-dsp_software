#include "FrequencyMeasurement.h"

#include "biquad.h"

#include "AudioStream.h"
FrequencyMeasurement::FrequencyMeasurement(int32_t noSamplerPerUpdate){
	_samplesAcc=noSamplerPerUpdate*_durations.size();
	getCoefficients(_biquadCoeffs, BiquadType::LOW_PASS, 0., 5., AUDIO_SAMPLE_RATE_EXACT/AUDIO_BLOCK_SAMPLES, 0.7);
	_lpFilter.pCoeffs=_biquadCoeffs;
	_lpFilter.numStages=1;
	_lpFilter.pState=_biquadState;
	_durationsId=0;
	reset();
}

void FrequencyMeasurement::reset(){
	_fLastValid=-1.f;
	_f=-1.f;
	_fillId=0;
	_durationAcc=0;
	_microsFirst=0;
	_oldFrequency=-1.f;
	_firstDer[0]=-1.f;
	_firstDer[1]=-1.f;
	for (uint8_t i =0; i< _durations.size(); i++){
		_durations[i]=0.;
	}
}

float FrequencyMeasurement::getFrequency() const {
	return _f;
}

float FrequencyMeasurement::getLastValidFrequency() const{
	return _fLastValid;
}
void FrequencyMeasurement::computeFrequency(){
    __disable_irq();
	if (_fillId < _durations.size()+1){
		__enable_irq();
		_f= -1.f;
		return;
	}
	float f=_samplesAcc/_durationAcc;
	if (_fillId==_durations.size()+1){
		preload(&_lpFilter,(double)f);
		_fillId++;
	}
    __enable_irq();
	biquad_cascade_df2T<float, arm_biquad_cascade_df2T_instance_f64, double>(&_lpFilter, &f, &f, 1);	
	f*=1e6; //micros to seconds
	if(_oldFrequency==-1.f){
		_oldFrequency=f;
		_f= -1.f;
		return;
	}
	float der=f-_oldFrequency;
	_oldFrequency=f;
	_firstDer[0]=_firstDer[1];
	if (_firstDer[1]==-1.f){
		_firstDer[1]=der;
		_f= -1.f;
		return;
	}
	else {
    	_firstDer[1]=0.5*_firstDer[1]+0.5*der;
	}
	if (_firstDer[0]==-1.f){
		_f= -1.f;
		return;
	}
	float secondDer=_firstDer[0]-_firstDer[1];
    // Serial.print(secondDer*1e6/f);
    // Serial.print(" ");
    // Serial.println(_firstDer[1]*1e6/f);
	// if (abs(_firstDer[1]*1e6/f) > 2.f || abs(secondDer*1e6/f) > 0.7f){
	// 	return -1.f;
	// }
	if (abs(_firstDer[1]*1e6/f) > 1.f || abs(secondDer*1e6/f) > 0.5f){
		_f= -1.f;
		return;
	}
    _f= f;
	_fLastValid=f;
}
uint32_t FrequencyMeasurement::getTimeOfLastUpdate() const {
	uint32_t m=_microsFirst;
	return m;
}
void FrequencyMeasurement::update(){
    	uint32_t m=micros();
		if(_fillId==0){
			_microsFirst=m;
		}
		else {
			_durationAcc-=_durations[_durationsId];
			_durations[_durationsId]=m-_microsFirst,			
			_durationAcc+=_durations[_durationsId];
			_durationsId=(_durationsId+1)%_durations.size();
			_microsFirst=m;
		}
		if(_fillId < _durations.size()+1){
			_fillId++;
		}
}