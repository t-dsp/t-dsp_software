#ifndef frequency_measurement_h_
#define frequency_measurement_h_
#include "Arduino.h"
#include <arm_math.h>
#include <array>

class FrequencyMeasurement
{
    public:
        FrequencyMeasurement(int32_t noSamplerPerUpdate);
        //@return a negativ value indicates that no valid frequency measurement was possible
        float getFrequency() const;
        float getLastValidFrequency() const;
        uint32_t getTimeOfLastUpdate() const;
        void computeFrequency();
        void update();
        void reset();
    private:

        arm_biquad_cascade_df2T_instance_f64 _lpFilter;
        double _biquadCoeffs[5];
        float _oldFrequency;
        float _firstDer[2];
        float _f;
        float _fLastValid;
        double _biquadState[2]={0.f, 0.f};
        uint8_t _fillId;
        uint8_t _durationsId;

        std::array<int32_t,25> _durations;

	    uint32_t _microsFirst;
        float _samplesAcc;
        float _durationAcc;

};

#endif