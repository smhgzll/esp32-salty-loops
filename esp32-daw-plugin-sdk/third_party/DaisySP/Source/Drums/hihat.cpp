#include "dsp.h"
#include "hihat.h"
#include <math.h>

using namespace daisysp;

void SquareNoise::Init(float sample_rate)
{
    last_f0_ = -1.0f;
    for(int i = 0; i < 6; i++)
    {
        phase_[i] = 0;
    }
}

float SquareNoise::Process(float f0)
{
    // esp32-daw-plugin-sdk: a static ratio table and in-place phase updates (the local table and
    // phase copies cost two memcpy calls per sample on the ESP32-S3); same arithmetic.
    static const float ratios[6] = {// Nominal f0: 414 Hz
                                    1.0f,
                                    1.304f,
                                    1.466f,
                                    1.787f,
                                    1.932f,
                                    2.536f};

    if(f0 != last_f0_)
    {
        for(int i = 0; i < 6; ++i)
        {
            float f = f0 * ratios[i];
            if(f >= 0.499f)
                f = 0.499f;
            increment_[i] = static_cast<uint32_t>(f * 4294967296.0f);
        }
        last_f0_ = f0;
    }
    uint32_t noise = 0;
    for(int i = 0; i < 6; ++i)
    {
        phase_[i] += increment_[i];
        noise += (phase_[i] >> 31);
    }

    return 0.33f * static_cast<float>(noise) - 1.0f;
}

void RingModNoise::Init(float sample_rate)
{
    sample_rate_ = sample_rate;

    for(int i = 0; i < 6; ++i)
    {
        oscillator_[i].Init(sample_rate_);
    }
}

float RingModNoise::Process(float f0)
{
    const float ratio = f0 / (0.01f + f0);
    const float f1a   = 200.0f / sample_rate_ * ratio;
    const float f1b   = 7530.0f / sample_rate_ * ratio;
    const float f2a   = 510.0f / sample_rate_ * ratio;
    const float f2b   = 8075.0f / sample_rate_ * ratio;
    const float f3a   = 730.0f / sample_rate_ * ratio;
    const float f3b   = 10500.0f / sample_rate_ * ratio;

    float out = ProcessPair(&oscillator_[0], f1a, f1b);
    out += ProcessPair(&oscillator_[2], f2a, f2b);
    out += ProcessPair(&oscillator_[4], f3a, f3b);

    return out;
}

float RingModNoise::ProcessPair(Oscillator* osc, float f1, float f2)
{
    osc[0].SetWaveform(Oscillator::WAVE_SQUARE);
    osc[0].SetFreq(f1 * sample_rate_);
    float temp_1 = osc[0].Process();

    osc[1].SetWaveform(Oscillator::WAVE_SAW);
    osc[1].SetFreq(f2 * sample_rate_);
    float temp_2 = osc[1].Process();

    return temp_1 * temp_2;
}
