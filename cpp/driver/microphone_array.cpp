/*
 * Copyright 2016 <Admobilize>
 * MATRIX Labs  [http://creator.matrix.one]
 * This file is part of MATRIX Creator HAL
 *
 * MATRIX Creator HAL is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <wiringPi.h>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <valarray>

#include "cpp/driver/creator_memory_map.h"
#include "cpp/driver/fir_coeff.h"
#include "cpp/driver/microphone_array.h"
#include "cpp/driver/microphone_array_location.h"

static std::mutex irq_m;
static std::condition_variable irq_cv;

void irq_callback(void) { irq_cv.notify_all(); }

namespace matrix_hal {

MicrophoneArray::MicrophoneArray()
    : lock_(irq_m), gain_(3), sampling_frequency_(16000) {
  raw_data_.resize(kMicarrayBufferSize);

  delayed_data_.resize(kMicarrayBufferSize);

  fifos_.resize(kMicrophoneChannels);

  beamformed_.resize(NumberOfSamples());

  fir_coeff_.resize(kNumberFIRTaps);

  CalculateDelays(0.0, 0.0);
}

MicrophoneArray::~MicrophoneArray() {}

void MicrophoneArray::Setup(MatrixIOBus *bus) {
  MatrixDriver::Setup(bus);

  wiringPiSetup();

  pinMode(kMicrophoneArrayIRQ, INPUT);
  wiringPiISR(kMicrophoneArrayIRQ, INT_EDGE_BOTH, &irq_callback);

  ReadConfValues();
  SelectFIRCoeff(sampling_frequency_);
}

//  Read audio from the FPGA and calculate beam using delay & sum method
bool MicrophoneArray::Read() {
  // TODO(andres.calderon@admobilize.com): avoid double buffer
  if (!bus_) return false;

  irq_cv.wait(lock_);

  if (!bus_->Read(kMicrophoneArrayBaseAddress,
                  reinterpret_cast<unsigned char *>(&raw_data_[0]),
                  sizeof(int16_t) * kMicarrayBufferSize)) {
    return false;
  }

  for (uint32_t s = 0; s < NumberOfSamples(); s++) {
    int sum = 0;
    for (int c = 0; c < kMicrophoneChannels; c++) {
      // delaying data for beamforming 'delay & sum' algorithm
      delayed_data_[s * kMicrophoneChannels + c] =
          fifos_[c].PushPop(raw_data_[c * NumberOfSamples() + s]);

      // accumulation data for beamforming 'delay & sum' algorithm
      sum += delayed_data_[s * kMicrophoneChannels + c];
    }

    beamformed_[s] = std::min(INT16_MAX, std::max(sum, INT16_MIN));
  }
  return true;
}

// Setting fifos for the 'delay & sum' algorithm
void MicrophoneArray::CalculateDelays(float azimutal_angle, float polar_angle,
                                      float radial_distance_mm,
                                      float sound_speed_mmseg) {
  if (sound_speed_mmseg == 0) {
    std::cerr << "Bad Configuration, Sound Speed must be greather than 0"
              << std::endl;
    return;
  }
  //  sound source position
  float x, y, z;
  x = radial_distance_mm * std::sin(azimutal_angle) * std::cos(polar_angle);
  y = radial_distance_mm * std::sin(azimutal_angle) * std::sin(polar_angle);
  z = radial_distance_mm * std::cos(azimutal_angle);

  std::map<float, int> distance_map;

  // sorted distances from source position to each microphone
  for (int c = 0; c < kMicrophoneChannels; c++) {
    const float distance = std::sqrt(
        std::pow(micarray_location[c][0] - x, 2.0) +
        std::pow(micarray_location[c][1] - y, 2.0) + std::pow(z, 2.0));
    distance_map[distance] = c;
  }

  // fifo resize for delay compensation
  float min_distance = distance_map.begin()->first;
  for (std::map<float, int>::iterator it = distance_map.begin();
       it != distance_map.end(); ++it) {
    int delay = std::round((it->first - min_distance) * sampling_frequency_ /
                           sound_speed_mmseg);
    fifos_[it->second].Resize(delay);
  }
}

bool MicrophoneArray::GetGain() {
  if (!bus_) return false;
  uint16_t value;
  bus_->Read(kConfBaseAddress + 0x07, &value);
  gain_ = value;
  return true;
}

bool MicrophoneArray::SetGain(uint16_t gain) {
  if (!bus_) return false;
  bus_->Write(kConfBaseAddress + 0x07, gain);
  gain_ = gain;
  return true;
}

bool MicrophoneArray::SetSamplingRate(uint32_t sampling_frequency) {
  if (sampling_frequency == 0) {
    std::cerr << "Bad Configuration, sampling frequency must be greather than 0"
              << std::endl;
    return false;
  }

  uint16_t MIC_gain, MIC_constant;
  for (int i = 0;; i++) {
    if (MIC_sampling_frequencies[i][0] == 0) {
      std::cerr << "Unsoported sampling frequency, it must be: 8000, 12000, "
                   "16000, 22050, 24000, 32000, 44100, 48000, 96000"
                << std::endl;
      return false;
    }
    if (sampling_frequency == MIC_sampling_frequencies[i][0]) {
      sampling_frequency_ = sampling_frequency;
      MIC_constant = MIC_sampling_frequencies[i][1];
      MIC_gain = MIC_sampling_frequencies[i][2];
      break;
    }
  }
  SelectFIRCoeff(sampling_frequency_);
  SetGain(MIC_gain);
  bus_->Write(kConfBaseAddress + 0x06, MIC_constant);

  return true;
}

bool MicrophoneArray::GetSamplingRate() {
  if (!bus_) return false;
  uint16_t value;
  bus_->Read(kConfBaseAddress + 0x06, &value);

  for (int i = 0;; i++) {
    if (MIC_sampling_frequencies[i][0] == 0) return false;
    if (value == MIC_sampling_frequencies[i][0]) {
      sampling_frequency_ = MIC_sampling_frequencies[i][0];
      break;
    }
  }

  return true;
}

void MicrophoneArray::ReadConfValues() {
  GetGain();
  GetSamplingRate();
}

void MicrophoneArray::ShowConfiguration() {
  std::cout << "Audio Configuration: " << std::endl;
  std::cout << "Sampling Frequency: " << sampling_frequency_ << std::endl;
  std::cout << "Gain : " << gain_ << std::endl;
}

bool MicrophoneArray::SetFIRCoeff() {
  return bus_->Write(kMicrophoneArrayBaseAddress,
                     reinterpret_cast<unsigned char *>(&fir_coeff_[0]),
                     fir_coeff_.size());
}

bool MicrophoneArray::SetCustomFIRCoeff(
    const std::valarray<int16_t> custom_fir) {
  if (custom_fir.size() == kNumberFIRTaps) {
    fir_coeff_ = custom_fir;
    return SetFIRCoeff();
  } else {
    std::cout << "Size FIR Filter must be : " << kNumberFIRTaps << std::endl;
    return false;
  }
}

bool MicrophoneArray::SelectFIRCoeff(uint32_t sampling_frequency) {
  if (sampling_frequency == 0) {
    std::cerr << "Bad Configuration, sampling_frequency must be greather than 0"
              << std::endl;
    return false;
  }

  for (int i = 0;; i++) {
    if (FIR_Coeff[i].rate_ == 0) {
      std::cerr << "Unsoported sampling frequency, it must be: 8000, 12000, "
                   "16000, 22050, 24000, 32000, 44100, 48000, 96000 "
                << std::endl;
      return false;
    }
    if (FIR_Coeff[i].rate_ == sampling_frequency) {
      fir_coeff_ = FIR_Coeff[i].coeff_;
      return SetFIRCoeff();
    }
  }
}
};  // namespace matrix_hal
