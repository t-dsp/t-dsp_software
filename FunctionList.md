# List of functions
This is a list of functions (aka OSC methods) which are available for each object type.

#### AsyncAudioInputSPDIF3
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| begin | void | b* | -- |  |
| getAttenuation | double | getA* | -- |  |
| getBufferedTime | double | getB* | -- |  |
| getHalfFilterLength | int32_t | getH* | -- |  |
| getInputFrequency | double | getI* | -- |  |
| getTargetLantency | double | getT* | -- |  |
| isLocked | static bool | i* | -- |  |
#### AudioAmplifier
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| gain | void | g* | f | float n |
| slew | void | s* | ; | bool doSlew |
#### AudioAnalyzeEvent
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| getCount | uint32_t | getC* | -- | void |
| getMicros | uint32_t | getM* | -- | void |
#### AudioAnalyzeFFT1024
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| available | bool | ava* | -- |  |
| averageTogether | void | ave* | i | uint8_t n |
| read | float | r* | ii | unsigned int binFirst, unsigned int binLast |
| read | float | r* | i | unsigned int binNumber |
| windowFunction | void | w* | s | string selecting one of the built-in window types, e.g. "Hanning" |
#### AudioAnalyzeFFT256
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| available | bool | ava* | -- |  |
| averageTogether | void | ave* | i | uint8_t n |
| read | float | r* | ii | unsigned int binFirst, unsigned int binLast |
| read | float | r* | i | unsigned int binNumber |
| windowFunction | void | w* | s | string selecting one of the built-in window types, e.g. "BlackmanHarris" |
#### AudioAnalyzeNoteFrequency
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| available | bool | a* | -- |  void  |
| begin | void | b* | f |  float threshold  |
| probability | float | p* | -- |  void  |
| read | float | r* | -- |  void  |
| threshold | void | t* | f |  float p  |
#### AudioAnalyzePeak
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| available | bool | a* | -- | void |
| readPeakToPeak | float | readP* | -- | void |
| read | float | read | -- | void |
#### AudioAnalyzePrint
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| delay | void | d* | i | uint32_t num |
| length | void | l* | i | uint32_t num |
| name | void | n* | s | const char *str |
| trigger | void | t* | -- | void |
#### AudioAnalyzeRMS
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| available | bool | a* | -- | void |
| read | float | r* | -- | void |
#### AudioAnalyzeToneDetect
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| available | bool | a* | -- | void |
| frequency | void | f* | fi | float freq, uint16_t cycles |
| read | float | r* | -- | void |
| set_params | void | s* | iii | int32_t coef, uint16_t cycles, uint16_t len |
| threshold | void | t* | f | float level |
#### AudioControlAK4558
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| disableIn | bool | disableI* | -- | void |
| disableOut | bool | disableO* | -- | void |
| disable | bool | disable | -- | void |
| enableIn | bool | enableI* | -- | void |
| enableOut | bool | enableO* | -- | void |
| enable | bool | enable | -- | void |
| inputLevel | bool | inputL* | f | float n |
| inputSelect | bool | inputS* | i | int n |
| volumeLeft | bool | volumeL* | f | float n |
| volumeRight | bool | volumeR* | f | float n |
| volume | bool | volume | f | float n |
#### AudioControlCS42448
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| disable | bool | d* | -- | void |
| enable | bool | e* | -- | void |
| filterFreeze | bool | f* | -- | void |
| inputLevel | bool | inputL* | f | float level |
| inputLevel | bool | inputL* | if | int channel, float level |
| inputSelect | bool | inputS* | i | int n |
| invertADC | bool | invertA* | i | uint32_t data |
| invertDAC | bool | invertD* | i | uint32_t data |
| setAddress | void | s* | i | uint8_t addr |
| volume | bool | v* | f | float level |
| volume | bool | v* | if | int channel, float level |
#### AudioControlCS4272
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| dacVolume | bool | da* | ff | float left, float right |
| dacVolume | bool | da* | f | float n |
| disableDither | bool | disableD* | -- | void |
| disable | bool | disable | -- | void |
| enableDither | bool | enableD* | -- | void |
| enable | bool | enable | -- | void |
| inputLevel | bool | inputL* | f | float n |
| inputSelect | bool | inputS* | i | int n |
| muteInput | bool | muteI* | -- | void |
| muteOutput | bool | muteO* | -- | void |
| unmuteInput | bool | unmuteI* | -- | void |
| unmuteOutput | bool | unmuteO* | -- | void |
| volume | bool | v* | ff | float left, float right |
| volume | bool | v* | f | float n |
#### AudioControlSGTL5000
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| adcHighPassFilterDisable | unsigned short | adcHighPassFilterD* | -- | void |
| adcHighPassFilterEnable | unsigned short | adcHighPassFilterE* | -- | void |
| adcHighPassFilterFreeze | unsigned short | adcHighPassFilterF* | -- | void |
| audioPostProcessorEnable | unsigned short | audioPo* | -- | void |
| audioPreProcessorEnable | unsigned short | audioPre* | -- | void |
| audioProcessorDisable | unsigned short | audioPro* | -- | void |
| autoVolumeControl | unsigned short | autoVolumeC* | iiifff | uint8_t maxGain, uint8_t lbiResponse, uint8_t hardLimit, float threshold, float attack, float decay |
| autoVolumeDisable | unsigned short | autoVolumeD* | -- | void |
| autoVolumeEnable | unsigned short | autoVolumeE* | -- | void |
| dacVolumeRampDisable | bool | dacVolumeRampD* | -- |  |
| dacVolumeRampLinear | bool | dacVolumeRampL* | -- |  |
| dacVolumeRamp | bool | dacVolumeRamp | -- |  |
| dacVolume | unsigned short | dacVolume | ff | float left, float right |
| dacVolume | unsigned short | dacVolume | f | float n |
| disable | bool | di* | -- | void |
| enable | bool | ena* | ii | const unsigned extMCLK, const uint32_t pllFreq  |
| enable | bool | ena* | -- | void |
| enhanceBassDisable | unsigned short | enhanceBassD* | -- | void |
| enhanceBassEnable | unsigned short | enhanceBassE* | -- | void |
| enhanceBass | unsigned short | enhanceBass | ff | float lr_lev, float bass_lev |
| enhanceBass | unsigned short | enhanceBass | ffii | float lr_lev, float bass_lev, uint8_t hpf_bypass, uint8_t cutoff |
| eqBands | void | eqBands | fffff | float bass, float mid_bass, float midrange, float mid_treble, float treble |
| eqBands | void | eqBands | ff | float bass, float treble |
| eqBand | unsigned short | eqBand | if | uint8_t bandNum, float n |
| eqFilterCount | unsigned short | eqFilterC* | i | uint8_t n |
| eqFilter | void | eqFilter | ib | filter number; blob holding 7 int32 parameters |
| eqSelect | unsigned short | eqS* | i | uint8_t n |
| headphoneSelect | bool | h* | i | int n |
| inputLevel | bool | inputL* | f | float n |
| inputSelect | bool | inputS* | i | int n |
| killAutomation | void | k* | -- | void |
| lineInLevel | bool | lineI* | ii | uint8_t left, uint8_t right |
| lineInLevel | bool | lineI* | i | uint8_t n |
| lineOutLevel | unsigned short | lineO* | ii | uint8_t left, uint8_t right |
| lineOutLevel | unsigned short | lineO* | i | uint8_t n |
| micGain | bool | mi* | i | unsigned int dB |
| muteHeadphone | bool | muteH* | -- | void |
| muteLineout | bool | muteL* | -- | void |
| setAddress | void | se* | i | uint8_t level |
| surroundSoundDisable | unsigned short | surroundSoundD* | -- | void |
| surroundSoundEnable | unsigned short | surroundSoundE* | -- | void |
| surroundSound | unsigned short | surroundSound | i | uint8_t width |
| surroundSound | unsigned short | surroundSound | ii | uint8_t width, uint8_t select |
| unmuteHeadphone | bool | unmuteH* | -- | void |
| unmuteLineout | bool | unmuteL* | -- | void |
| volume | bool | v* | ff | float left, float right |
| volume | bool | v* | f | float n |
#### AudioControlTLV320AIC3206
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| aic_readPage | unsigned int | aic_r* | ii | uint8_t page, uint8_t reg |
| aic_writePage | bool | aic_w* | iii | uint8_t page, uint8_t reg, uint8_t val |
| disable | bool | d* | -- | void |
| enableAutoMuteDAC | bool | enableA* | ;i | bool, uint8_t |
| enableMicDetect | bool | enableM* | ; | bool |
| enable | bool | enable | -- | void |
| getHPCutoff_Hz | float | getH* | -- | void |
| getSampleRate_Hz | float | getS* | -- | void |
| inputLevel | bool | inputL* | f | float n |
| inputSelect | bool | inputS* | i | int n |
| outputSelect | bool | o* | i | int n |
| readMicDetect | int | r* | -- | void |
| setHPFonADC | void | setH* | ;ff | bool enable, float cutoff_Hz, float fs_Hz |
| setIIRCoeffOnADC | void | setII* | ib | filter channel; blob holding 3 uint32 coefficients |
| setInputGain_dB | bool | setIn* | f | float n |
| setMicBias | bool | setM* | i | int n |
| updateInputBasedOnMicDetect | bool | u* | i | int setting  |
| volume_dB | bool | volume_* | f | float n |
| volume | bool | volume | f | float n |
#### AudioControlWM8731
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| disable | bool | d* | -- | void |
| enable | bool | e* | -- | void |
| inputLevel | bool | inputL* | f | float n |
| inputSelect | bool | inputS* | i | int n |
| volume | bool | v* | f | float n |
#### AudioControlWM8731master
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| enable | bool | e* | -- | void |
#### AudioEffectBitcrusher
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| bits | void | b* | i | uint8_t b |
| sampleRate | void | s* | f | float hz |
#### AudioEffectDelay
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| delay | void | de* | if | uint8_t channel, float milliseconds |
| disable | void | di* | i | uint8_t channel |
#### AudioEffectDelayExternal
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| delay | void | de* | if | uint8_t channel, float milliseconds |
| disable | void | di* | i | uint8_t channel |
#### AudioEffectDigitalCombine
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| setCombineMode | void | s* | i | int mode_in |
#### AudioEffectEnvelope
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| attack | void | a* | f | float milliseconds |
| decay | void | dec* | f | float milliseconds |
| delay | void | del* | f | float milliseconds |
| hold | void | h* | f | float milliseconds |
| isActive | bool | isA* | -- |  |
| isSustain | bool | isS* | -- |  |
| noteOff | void | noteOf* | -- |  |
| noteOn | void | noteOn | -- |  |
| releaseNoteOn | void | releaseN* | f | float milliseconds |
| release | void | release | f | float milliseconds |
| sustain | void | s* | f | float level |
#### AudioEffectExpEnvelope
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| attack | void | a* | ff | float milliseconds, float target_factor  |
| close | void | c* | -- |  |
| decay | void | dec* | ff | float milliseconds, float target_factor  |
| delay | void | del* | f | float milliseconds |
| getGain | float | getG* | -- |  |
| getState | uint8_t | getS* | -- |  |
| hold | void | h* | f | float milliseconds |
| isActive | bool | isA* | -- |  |
| isSustain | bool | isS* | -- |  |
| noteOff | void | noteOf* | -- |  |
| noteOn | void | noteOn | -- |  |
| release | void | r* | ff | float milliseconds, float target_factor  |
| sustain | void | s* | f | float level |
#### AudioEffectFade
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| fadeIn | void | fadeI* | i | uint32_t milliseconds |
| fadeOut | void | fadeO* | i | uint32_t milliseconds |
#### AudioEffectFreeverb
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| damping | void | d* | f | float n |
| roomsize | void | r* | f | float n |
#### AudioEffectFreeverbStereo
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| damping | void | d* | f | float n |
| roomsize | void | r* | f | float n |
#### AudioEffectGranular
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| beginFreeze | void | beginF* | f | float grain_length |
| beginPitchShift | void | beginP* | f | float grain_length |
| begin | void | begin | bi | blob holding 16-bit grain data; number of samples in data |
| setSpeed | void | se* | f | float ratio |
| stop | void | st* | -- |  |
#### AudioEffectMidSide
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| decode | void | d* | -- |  |
| encode | void | e* | -- |  |
#### AudioEffectReverb
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| reverbTime | void | r* | f | float |
#### AudioFilterBiquad
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| setBandpass | void | setB* | iff | uint32_t stage, float frequency, float q  |
| setCoefficients | void | setC* | iddddd | filter stage number, followed by 5 coefficient values |
| setCoefficients | void | setC* | iiiiii | filter stage number, followed by 5 coefficient values |
| setHighShelf | void | setHighS* | ifff | uint32_t stage, float frequency, float gain, float slope  |
| setHighpass | void | setHighp* | iff | uint32_t stage, float frequency, float q  |
| setLowShelf | void | setLowS* | ifff | uint32_t stage, float frequency, float gain, float slope  |
| setLowpass | void | setLowp* | iff | uint32_t stage, float frequency, float q  |
| setNotch | void | setN* | iff | uint32_t stage, float frequency, float q  |
#### AudioFilterFIR
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| end | void | e* | -- | void |
#### AudioFilterLadder
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| frequency | void | f* | f | float FC |
| inputDrive | void | inp* | f | float drv |
| interpolationMethod | void | int* | i | AudioFilterLadderInterpolation im |
| octaveControl | void | o* | f | float octaves |
| passbandGain | void | p* | f | float passbandgain |
| resonance | void | r* | f | float reson |
#### AudioFilterStateVariable
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| frequency | void | f* | f | float freq |
| octaveControl | void | o* | f | float n |
| resonance | void | r* | f | float q |
#### AudioInputSPDIF3
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| pllLocked | static bool | p* | -- | void |
| sampleRate | static unsigned int | s* | -- | void |
#### AudioMixer
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| gain | void | ga* | f | float gain |
| gain | void | ga* | if | unsigned int channel, float gain |
| getChannels | uint8_t | ge* | -- | void |
#### AudioMixer4
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| gain | void | g* | if | unsigned int channel, float gain |
#### AudioMixerStereo
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| balance | void | b* | if | unsigned int chLeft, float bal |
| balance | void | b* | iif | unsigned int chLeft, unsigned int chRight, float bal |
| gain | void | ga* | f | float gain |
| gain | void | ga* | if | unsigned int channel, float gain |
| getChannels | uint8_t | ge* | -- | void |
| pan | void | p* | if | unsigned int channel, float pan |
| setPanLaw | void | s* | f | float law |
#### AudioOutputADAT
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| mute_PCM | static void | m* | ; | const bool mute |
#### AudioOutputAnalog
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| analogReference | void | a* | i | int ref |
#### AudioOutputAnalogStereo
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| analogReference | void | a* | i | int ref |
#### AudioOutputSPDIF
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| mute_PCM | static void | m* | ; | const bool mute |
#### AudioOutputSPDIF2
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| mute_PCM | static void | m* | ; | const bool mute |
#### AudioOutputSPDIF3
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| mute_PCM | static void | m* | ; | const bool mute |
#### AudioPlayMemory
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| isPlaying | bool | i* | -- | void |
| lengthMillis | uint32_t | l* | -- | void |
| play | void | pl* | i | 32-bit integer pointing to valid data built in to sketch |
| positionMillis | uint32_t | po* | -- | void |
| stop | void | s* | -- | void |
#### AudioPlayQueue
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| available | bool | a* | -- | void |
| getBuffer | int16_t * | g* | -- | void |
| playBuffer | uint32_t | playB* | -- | void |
| play | uint32_t | play | i | int16_t data |
| setBehaviour | void | setB* | i | behaviour_e behave |
| setMaxBuffers | void | setM* | i | uint8_t |
#### AudioPlaySdRaw
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| begin | void | b* | -- | void |
| isPlaying | bool | i* | -- | void |
| lengthMillis | uint32_t | l* | -- | void |
| play | bool | pl* | s | const char *filename |
| positionMillis | uint32_t | po* | -- | void |
| stop | void | s* | -- | void |
#### AudioPlaySdWav
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| begin | void | b* | -- | void |
| isPaused | bool | isPa* | -- | void |
| isPlaying | bool | isPl* | -- | void |
| isStopped | bool | isS* | -- | void |
| lengthMillis | uint32_t | l* | -- | void |
| play | bool | pl* | s | const char *filename |
| positionMillis | uint32_t | po* | -- | void |
| stop | void | s* | -- | void |
| togglePlayPause | void | t* | -- | void |
#### AudioPlaySerialflashRaw
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| begin | void | b* | -- | void |
| isPlaying | bool | i* | -- | void |
| lengthMillis | uint32_t | l* | -- | void |
| play | bool | pl* | s | const char *filename |
| positionMillis | uint32_t | po* | -- | void |
| stop | void | s* | -- | void |
#### AudioRecordQueue
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| available | int | a* | -- | void |
| begin | void | b* | -- | void |
| clear | void | c* | -- | void |
| end | void | e* | -- | void |
| freeBuffer | void | f* | -- | void |
| readBuffer | int16_t * | r* | -- | void |
#### AudioSynthKarplusStrong
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| noteOff | void | noteOf* | f | float velocity |
| noteOn | void | noteOn | ff | float frequency, float velocity |
#### AudioSynthNoisePink
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| amplitude | void | a* | f | float n |
#### AudioSynthNoiseWhite
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| amplitude | void | a* | f | float n |
#### AudioSynthSimpleDrum
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| frequency | void | f* | f | float freq |
| length | void | l* | i | int32_t milliseconds |
| noteOn | void | n* | -- |  |
| pitchMod | void | p* | f | float depth |
| secondMix | void | s* | f | float level |
#### AudioSynthToneSweep
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| isPlaying | unsigned char | i* | -- | void |
| play | boolean | p* | fiif | float t_amp,int t_lo,int t_hi,float t_time |
| read | float | r* | -- | void |
#### AudioSynthWaveform
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| amplitude | void | am* | f | float n |
| arbitraryWaveform | void | ar* | bf | blob containing 256 samples; float is currently unused |
| begin | void | b* | ffi | float t_amp, float t_freq, short t_type |
| begin | void | b* | i | short t_type |
| frequency | void | f* | f | float freq |
| offset | void | o* | f | float n |
| phase | void | ph* | f | float angle |
| pulseWidth | void | pu* | f | float n |
#### AudioSynthWaveformDc
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| amplitude | void | a* | f | float n |
| amplitude | void | a* | ff | float n, float milliseconds |
| read | float | r* | -- | void |
#### AudioSynthWaveformModulated
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| amplitude | void | am* | f | float n |
| arbitraryWaveform | void | ar* | bf | blob containing 256 samples; float is currently unused |
| begin | void | b* | ffi | float t_amp, float t_freq, short t_type |
| begin | void | b* | i | short t_type |
| frequencyModulation | void | frequencyM* | f | float octaves |
| frequency | void | frequency | f | float freq |
| offset | void | o* | f | float n |
| phaseModulation | void | p* | f | float degrees |
#### AudioSynthWaveformPWM
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| amplitude | void | a* | f | float n |
| frequency | void | f* | f | float freq |
#### AudioSynthWaveformSine
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| amplitude | void | a* | f | float n |
| frequency | void | f* | f | float freq |
| phase | void | p* | f | float angle |
#### AudioSynthWaveformSineHires
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| amplitude | void | a* | f | float n |
| frequency | void | f* | f | float freq |
| phase | void | p* | f | float angle |
#### AudioSynthWaveformSineModulated
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| amplitude | void | a* | f | float n |
| frequency | void | f* | f | float freq |
| phase | void | p* | f | float angle |
#### AudioSynthWavetable
|function|return type|short-form|parameter pattern|parameters|
|----|----|----|----|----|
| amplitude | void | a* | f | float v |
| freqToNote | static int | f* | f | float freq |
| getEnvState | envelopeStateEnum | g* | -- | void |
| isPlaying | bool | i* | -- | void |
| midi_volume_transform | static float | m* | i | int midi_amp |
| noteToFreq | static float | n* | i | int note |
| playFrequency | void | playF* | fi | float freq, int amp  |
| playNote | void | playN* | ii | int note, int amp  |
| setFrequency | void | setF* | f | float freq |
| stop | void | st* | -- | void |