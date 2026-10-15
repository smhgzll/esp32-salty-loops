/*
Copyright (c) 2020 Electrosmith, Corp

Use of this source code is governed by an MIT-style
license that can be found in the LICENSE file or at
https://opensource.org/licenses/MIT.
*/

#pragma once
#ifndef DSY_SVF_H
#define DSY_SVF_H

#include "dsp.h" // esp32-daw-plugin-sdk: fclamp / SinPiQuarter for the inline definitions below

namespace daisysp
{
/**      Double Sampled, Stable State Variable Filter

Credit to Andrew Simper from musicdsp.org

This is his "State Variable Filter (Double Sampled, Stable)"

Additional thanks to Laurent de Soras for stability limit, and 
Stefan Diedrichsen for the correct notch output

Ported by: Stephen Hensley
*/
class Svf
{
  public:
    Svf() {}
    ~Svf() {}
    /** Initializes the filter
        float sample_rate - sample rate of the audio engine being run, and the frequency that the Process function will be called.
    */
    void Init(float sample_rate);


    /** 
        Process the input signal, updating all of the outputs.
    */
    // esp32-daw-plugin-sdk: defined in this header so the drum models' per-sample calls inline.
    inline void Process(float in);


    /** sets the frequency of the cutoff frequency. 
        f must be between 0.0 and sample_rate / 3
    */
    void SetFreq(float f);

    /** sets the resonance of the filter.
        Must be between 0.0 and 1.0 to ensure stability.
    */
    void SetRes(float r);

    // esp32-daw-plugin-sdk: set frequency and Plaits-style Q together (Q >= 0.51); inline, without
    // a libm call or a division by the sample rate (the analog drums call it every sample).
    inline void SetFreqAndQ(float f, float q);

    /** sets the drive of the filter 
        affects the response of the resonance of the filter
    */
    void SetDrive(float d);
    /** lowpass output
        \return low pass output of the filter
    */
    inline float Low() { return out_low_; }
    /** highpass output
        \return high pass output of the filter
    */
    inline float High() { return out_high_; }
    /** bandpass output
        \return band pass output of the filter
    */
    inline float Band() { return out_band_; }
    /** notchpass output
        \return notch pass output of the filter
    */
    inline float Notch() { return out_notch_; }
    /** peak output
        \return peak output of the filter
    */
    inline float Peak() { return out_peak_; }

  private:
    float sr_, fc_, res_, drive_, freq_, damp_;
    float notch_, low_, high_, band_, peak_;
    float input_;
    float out_low_, out_high_, out_band_, out_peak_, out_notch_;
    float pre_drive_, fc_max_;
    float inv_2sr_; // esp32-daw-plugin-sdk: 1 / (2 * sample rate), the double-sampled step
};

// esp32-daw-plugin-sdk: Process and SetFreqAndQ moved here from svf.cpp (unchanged arithmetic,
// except in SetFreqAndQ: SinPiQuarter instead of sinf, a multiplication by 1 / (2 sr) and FastDiv
// instead of divisions).
inline void Svf::Process(float in)
{
    input_ = in;
    // first pass
    notch_ = input_ - damp_ * band_;
    low_   = low_ + freq_ * band_;
    high_  = notch_ - low_;
    band_  = freq_ * high_ + band_ - drive_ * band_ * band_ * band_;
    // take first sample of output
    out_low_   = 0.5f * low_;
    out_high_  = 0.5f * high_;
    out_band_  = 0.5f * band_;
    out_peak_  = 0.5f * (low_ - high_);
    out_notch_ = 0.5f * notch_;
    // second pass
    notch_ = input_ - damp_ * band_;
    low_   = low_ + freq_ * band_;
    high_  = notch_ - low_;
    band_  = freq_ * high_ + band_ - drive_ * band_ * band_ * band_;
    // average second pass outputs
    out_low_ += 0.5f * low_;
    out_high_ += 0.5f * high_;
    out_band_ += 0.5f * band_;
    out_peak_ += 0.5f * (low_ - high_);
    out_notch_ += 0.5f * notch_;
}

inline void Svf::SetFreqAndQ(float f, float q)
{
    fc_ = fclamp(f, 1.0e-6f, fc_max_);
    const float x = fc_ * inv_2sr_;
    freq_ = 2.0f * SinPiQuarter(x < 0.25f ? x : 0.25f);
    q = q < 0.51f ? 0.51f : q;
    const float root = 1.0f - FastDiv(0.5f, q);
    res_ = root * root * root * root;
    // PlaitsQToSvfRes makes res_ = root^4; use root directly for the damping.
    damp_ = 2.0f * (1.0f - root);
    // fc <= sr / 3 keeps freq_ <= 1 and the stability bound >= 1.5.
    // For Q >= 1, damping <= 1, so the bound cannot limit it.
    if(q < 1.0f)
    {
        const float bound = FastDiv(2.0f, freq_) - freq_ * 0.5f;
        const float lim   = bound < 2.0f ? bound : 2.0f;
        damp_             = damp_ < lim ? damp_ : lim;
    }
    drive_ = pre_drive_ * res_;
}

} // namespace daisysp

#endif
