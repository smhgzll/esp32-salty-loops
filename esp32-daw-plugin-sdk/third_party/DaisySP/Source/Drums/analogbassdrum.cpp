#include "dsp.h"
#include "analogbassdrum.h"
#include <cmath>

using namespace daisysp;

void AnalogBassDrum::Init(float sample_rate)
{
    sample_rate_ = sample_rate;
    trigger_duration_ = static_cast<int>(1.0e-3f * sample_rate_);
    fm_duration_ = static_cast<int>(6.0e-3f * sample_rate_);
    pulse_decay_ = 1.0f - 1.0f / (0.2e-3f * sample_rate_);
    pulse_filter_ = 1.0f / (0.1e-3f * sample_rate_);
    retrig_decay_ = 1.0f - 1.0f / (0.05f * sample_rate_);
    coefficients_dirty_ = true;

    trig_ = false;

    pulse_remaining_samples_    = 0;
    fm_pulse_remaining_samples_ = 0;
    pulse_                      = 0.0f;
    pulse_height_               = 0.0f;
    pulse_lp_                   = 0.0f;
    fm_pulse_lp_                = 0.0f;
    retrig_pulse_               = 0.0f;
    lp_out_                     = 0.0f;
    tone_lp_                    = 0.0f;
    sustain_gain_               = 0.0f;
    phase_                      = 0.f;


    SetSustain(false);
    SetAccent(.1f);
    SetFreq(50.f);
    SetTone(.1f);
    SetDecay(.3f);
    SetSelfFmAmount(1.f);
    SetAttackFmAmount(.5f);

    resonator_.Init(sample_rate_);
}

inline float AnalogBassDrum::Diode(float x)
{
    if(x >= 0.0f)
    {
        return x;
    }
    else
    {
        x *= 2.0f;
        return FastDiv(0.7f * x, 1.0f + fabsf(x)); // esp32-daw-plugin-sdk: was a division
    }
}

float AnalogBassDrum::Process(bool trigger)
{
    if(coefficients_dirty_)
        UpdateCoefficients();


    if(trigger || trig_)
    {
        trig_ = false;

        pulse_remaining_samples_    = trigger_duration_;
        fm_pulse_remaining_samples_ = fm_duration_;
        pulse_height_               = 3.0f + 7.0f * accent_;
        lp_out_                     = 0.0f;
    }

    // Q39 / Q40
    float pulse = 0.0f;
    if(pulse_remaining_samples_)
    {
        --pulse_remaining_samples_;
        pulse = pulse_remaining_samples_ ? pulse_height_ : pulse_height_ - 1.0f;
        pulse_ = pulse;
    }
    else
    {
        pulse_ *= pulse_decay_;
        pulse = pulse_;
    }
    if(sustain_)
    {
        pulse = 0.0f;
    }

    // C40 / R163 / R162 / D83
    fonepole(pulse_lp_, pulse, pulse_filter_);
    pulse = Diode((pulse - pulse_lp_) + pulse * 0.044f);

    // Q41 / Q42
    float fm_pulse = 0.0f;
    if(fm_pulse_remaining_samples_)
    {
        --fm_pulse_remaining_samples_;
        fm_pulse = 1.0f;
        // C39 / C52
        retrig_pulse_ = fm_pulse_remaining_samples_ ? 0.0f : -0.8f;
    }
    else
    {
        // C39 / R161
        retrig_pulse_ *= retrig_decay_;
    }
    if(sustain_)
    {
        fm_pulse = 0.0f;
    }
    fonepole(fm_pulse_lp_, fm_pulse, pulse_filter_);

    // Q43 and R170 leakage
    float punch = 0.7f + Diode(10.0f * lp_out_ - 1.0f);

    // Q43 / R165
    float attack_fm = fm_pulse_lp_ * 1.7f * attack_fm_amount_;
    float self_fm   = punch * 0.08f * self_fm_amount_;
    float f         = f0_ * (1.0f + attack_fm + self_fm);
    f               = fclamp(f, 0.0f, 0.4f);

    float resonator_out;
    if(sustain_)
    {
        sustain_gain_ = accent_ * decay_;
        phase_ += f;
        phase_ = phase_ >= 1.f ? phase_ - 1.f : phase_;

        resonator_out = sinf(TWOPI_F * phase_) * sustain_gain_; // esp32-daw-plugin-sdk: float (was double sin / cos)
        lp_out_       = cosf(TWOPI_F * phase_) * sustain_gain_;
    }
    else
    {
        // Update both controls directly from Q, avoiding a redundant Q -> res -> Q conversion.
        resonator_.SetFreqAndQ(f * sample_rate_, 1.0f + q_ * f);

        resonator_.Process((pulse - retrig_pulse_ * 0.2f) * scale_);
        resonator_out = resonator_.Band();
        lp_out_       = resonator_.Low();
    }

    fonepole(tone_lp_, pulse * exciter_leak_ + resonator_out, tone_f_);

    return tone_lp_;
}

void AnalogBassDrum::UpdateCoefficients()
{
    scale_ = 0.001f / f0_;
    q_ = 1500.0f * powf(2.f, kOneTwelfth * decay_ * 80.0f);
    tone_f_ = fminf(4.0f * f0_ * powf(2.f, kOneTwelfth * tone_ * 108.0f), 1.0f);
    exciter_leak_ = 0.08f * (tone_ + 0.25f);
    coefficients_dirty_ = false;
}

void AnalogBassDrum::Trig()
{
    trig_ = true;
}

void AnalogBassDrum::SetSustain(bool sustain)
{
    sustain_ = sustain;
}

void AnalogBassDrum::SetAccent(float accent)
{
    accent_ = fclamp(accent, 0.f, 1.f);
}

void AnalogBassDrum::SetFreq(float f0)
{
    f0 /= sample_rate_;
    f0_ = fclamp(f0, 0.f, .5f);
    coefficients_dirty_ = true;
}

void AnalogBassDrum::SetTone(float tone)
{
    tone_ = fclamp(tone, 0.f, 1.f);
    coefficients_dirty_ = true;
}

void AnalogBassDrum::SetDecay(float decay)
{
    // esp32-daw-plugin-sdk: Plaits' 0..1 decay range again. Upstream squeezed it into -0.1..0
    // (q 943..1500), which only kept the undamped resonator (see Process) from ringing forever.
    decay_ = fclamp(decay, 0.f, 1.f);
    coefficients_dirty_ = true;
}

void AnalogBassDrum::SetAttackFmAmount(float attack_fm_amount)
{
    attack_fm_amount_ = attack_fm_amount * 50.f;
}

void AnalogBassDrum::SetSelfFmAmount(float self_fm_amount)
{
    self_fm_amount_ = self_fm_amount * 50.f;
}
