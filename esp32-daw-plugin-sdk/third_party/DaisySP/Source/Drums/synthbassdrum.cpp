#include "synthbassdrum.h"
#include <math.h>
#include <stdlib.h>

using namespace daisysp;

void SyntheticBassDrumClick::Init(float sample_rate)
{
    lp_ = 0.0f;
    hp_ = 0.0f;
    filter_.Init(sample_rate);
    filter_.SetFreq(5000.0f);
    filter_.SetRes(1.f); //2.f
}

float SyntheticBassDrumClick::Process(float in)
{
    //SLOPE(lp_, in, 0.5f, 0.1f);
    float error = in - lp_;
    lp_ += (error > 0 ? .5f : .1f) * error;

    fonepole(hp_, lp_, 0.04f);
    filter_.Process(lp_ - hp_);
    return filter_.Low();
}

void SyntheticBassDrumAttackNoise::Init()
{
    lp_ = 0.0f;
    hp_ = 0.0f;
}

float SyntheticBassDrumAttackNoise::Process()
{
    float sample = rand() * kRandFrac;
    fonepole(lp_, sample, 0.05f);
    fonepole(hp_, lp_, 0.005f);
    return lp_ - hp_;
}

void SyntheticBassDrum::Init(float sample_rate)
{
    sample_rate_ = sample_rate;
    transient_env_decay_ = 1.0f - 1.0f / (0.005f * sample_rate_);
    coefficients_dirty_ = true;

    trig_ = false;

    phase_                = 0.0f;
    phase_noise_          = 0.0f;
    f0_                   = 0.0f;
    fm_                   = 0.0f;
    fm_lp_                = 0.0f;
    body_env_lp_          = 0.0f;
    body_env_             = 0.0f;
    body_env_pulse_width_ = 0;
    fm_pulse_width_       = 0;
    tone_lp_              = 0.0f;
    sustain_gain_         = 0.0f;

    SetFreq(100.f);
    SetSustain(false);
    SetAccent(.2f);
    SetTone(.6f);
    SetDecay(.7f);
    SetDirtiness(.3f);
    SetFmEnvelopeAmount(.6);
    SetFmEnvelopeDecay(.3);

    click_.Init(sample_rate);
    noise_.Init();
}

inline float SyntheticBassDrum::DistortedSine(float phase,
                                              float phase_noise,
                                              float dirtiness)
{
    phase += phase_noise * dirtiness;

    //MAKE_INTEGRAL_FRACTIONAL(phase);
    int32_t phase_integral   = static_cast<int32_t>(phase);
    float   phase_fractional = phase - static_cast<float>(phase_integral);

    phase            = phase_fractional;
    float triangle   = (phase < 0.5f ? phase : 1.0f - phase) * 4.0f - 1.0f;
    float sine       = FastDiv(2.0f * triangle, 1.0f + fabsf(triangle)); // esp32-daw-plugin-sdk: was a division
    float clean_sine = SinTwoPi(phase + 0.75f); // esp32-daw-plugin-sdk: was sinf(TWOPI_F * (phase + 0.75f))
    return sine + (1.0f - dirtiness) * (clean_sine - sine);
}

inline float SyntheticBassDrum::TransistorVCA(float s, float gain)
{
    s = (s - 0.6f) * gain;
    return FastDiv(3.0f * s, 2.0f + fabsf(s)) + gain * 0.3f; // esp32-daw-plugin-sdk: was a division
}

float SyntheticBassDrum::Process(bool trigger)
{
    if(coefficients_dirty_)
        UpdateCoefficients();
    const float transient_level = tone_;

    if(trigger || trig_)
    {
        trig_     = false;
        fm_       = 1.0f;
        body_env_ = transient_env_ = 0.3f + 0.7f * accent_;
        body_env_pulse_width_      = sample_rate_ * 0.001f;
        fm_pulse_width_            = sample_rate_ * 0.0013f;
    }

    sustain_gain_ = accent_ * decay_;

    fonepole(phase_noise_, rand() * kRandFrac - 0.5f, 0.002f);

    float mix = 0.0f;

    if(sustain_)
    {
        f0_ = new_f0_;
        phase_ += f0_;
        if(phase_ >= 1.0f)
        {
            phase_ -= 1.0f;
        }
        float body = DistortedSine(phase_, phase_noise_, effective_dirtiness_);
        mix -= TransistorVCA(body, sustain_gain_);
    }
    else
    {
        if(fm_pulse_width_)
        {
            --fm_pulse_width_;
            phase_ = 0.25f;
        }
        else
        {
            fm_ *= fm_decay_;
            float fm = 1.0f + fm_envelope_amount_ * 3.5f * fm_lp_;
            f0_      = new_f0_;
            phase_ += fmin(f0_ * fm, 0.5f); // esp32-daw-plugin-sdk: inline, was a libm fminf call
            if(phase_ >= 1.0f)
            {
                phase_ -= 1.0f;
            }
        }

        if(body_env_pulse_width_)
        {
            --body_env_pulse_width_;
        }
        else
        {
            body_env_ *= body_env_decay_;
            transient_env_ *= transient_env_decay_;
        }

        const float envelope_lp_f = 0.1f;
        fonepole(body_env_lp_, body_env_, envelope_lp_f);
        fonepole(transient_env_lp_, transient_env_, envelope_lp_f);
        fonepole(fm_lp_, fm_, envelope_lp_f);

        float body      = DistortedSine(phase_, phase_noise_, effective_dirtiness_);
        float transient = click_.Process(body_env_pulse_width_ ? 0.0f : 1.0f)
                          + noise_.Process();

        mix -= TransistorVCA(body, body_env_lp_);
        mix -= transient * transient_env_lp_ * transient_level;
    }

    fonepole(tone_lp_, mix, tone_f_);
    return tone_lp_;
}

void SyntheticBassDrum::UpdateCoefficients()
{
    effective_dirtiness_ = dirtiness_ * fmaxf(1.0f - 8.0f * new_f0_, 0.0f);
    fm_decay_ = 1.0f - 1.0f / (0.008f * (1.0f + fm_envelope_decay_ * 4.0f) * sample_rate_);
    body_env_decay_ = 1.0f - 1.0f / (0.02f * sample_rate_)
                               * powf(2.f, (-decay_ * 60.0f) * kOneTwelfth);
    tone_f_ = fminf(4.0f * new_f0_ * powf(2.f, (tone_ * 108.0f) * kOneTwelfth), 1.0f);
    coefficients_dirty_ = false;
}

void SyntheticBassDrum::Trig()
{
    trig_ = true;
}

void SyntheticBassDrum::SetSustain(bool sustain)
{
    sustain_ = sustain;
}

void SyntheticBassDrum::SetAccent(float accent)
{
    accent_ = fclamp(accent, 0.f, 1.f);
}

void SyntheticBassDrum::SetFreq(float freq)
{
    freq /= sample_rate_;
    new_f0_ = fclamp(freq, 0.f, 1.f);
    coefficients_dirty_ = true;
}

void SyntheticBassDrum::SetTone(float tone)
{
    tone_ = fclamp(tone, 0.f, 1.f);
    coefficients_dirty_ = true;
}

void SyntheticBassDrum::SetDecay(float decay)
{
    decay  = fclamp(decay, 0.f, 1.f);
    decay_ = decay * decay;
    coefficients_dirty_ = true;
}

void SyntheticBassDrum::SetDirtiness(float dirtiness)
{
    dirtiness_ = fclamp(dirtiness, 0.f, 1.f);
    coefficients_dirty_ = true;
}

void SyntheticBassDrum::SetFmEnvelopeAmount(float fm_envelope_amount)
{
    fm_envelope_amount_ = fclamp(fm_envelope_amount, 0.f, 1.f);
}

void SyntheticBassDrum::SetFmEnvelopeDecay(float fm_envelope_decay)
{
    fm_envelope_decay  = fclamp(fm_envelope_decay, 0.f, 1.f);
    fm_envelope_decay_ = fm_envelope_decay * fm_envelope_decay;
    coefficients_dirty_ = true;
}
