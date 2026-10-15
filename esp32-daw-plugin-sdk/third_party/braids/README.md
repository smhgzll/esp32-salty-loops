# Braids (Mutable Instruments) for esp32-daw-plugin-sdk

The macro oscillator of Mutable Instruments Braids by Émilie Gillet, and the parts of stmlib it
uses. MIT (see LICENSE and the header of every file).

- eurorack: https://github.com/pichenettes/eurorack at 08460a69a7e1f7a81c5a2abcc7189c9a6b7208d4
  (braids/: macro_oscillator, analog_oscillator, digital_oscillator, resources, settings.h
  (the shape enum only), excitation.h, svf.h, parameter_interpolation.h)
- stmlib: https://github.com/pichenettes/stmlib at e3bd7c9cc00e4364166f9905c0509b6ffd0535ec
  (stmlib.h, utils/dsp.h, utils/random.h, utils/random.cc)

The files are unmodified (byte-identical to the revisions above; no patch). The plugin wrapper
(`plugins/macrosynth/macrosynth.cpp`) supplies the sample rate, block size and voice handling.
