// src/dji/dsp.hpp — internal DSP helpers for the DJI decoder (FFTW-backed).
// GPL-3.0-or-later.
#pragma once

#include <vector>

#include "droneid/types.h"

namespace droneid::dji::dsp {

using droneid::cf32;

// In-place-style transforms returning a new vector. ifft includes 1/N scaling (MATLAB convention).
std::vector<cf32> fft(const std::vector<cf32>& x);
std::vector<cf32> ifft(const std::vector<cf32>& x);
std::vector<cf32> fftshift(const std::vector<cf32>& x);

// Multiply x by exp(j*2*pi*(freq/fs)*n) for n=0.. (positive freq shifts spectrum up).
void freq_shift(std::vector<cf32>& x, double freq, double fs, double n0 = 0.0);

// FIR low-pass (windowed-sinc, Hamming), cutoff in Hz; filters x in place (causal, same length).
std::vector<cf32> fir_lowpass(const std::vector<cf32>& x, double cutoff_hz, double fs, int taps);

// Normalized cross-correlation score (0..1) of x against template zc, via FFT overlap-add.
// Returns length x.size()-zc.size()+1; score[i] corresponds to zc starting at x[i].
std::vector<float> normalized_corr(const std::vector<cf32>& x, const std::vector<cf32>& zc);

}  // namespace droneid::dji::dsp
