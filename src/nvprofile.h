#pragma once
#include <string>

// Sets the NVIDIA driver's "Power management mode" for this executable's own profile
// (Prefer maximum performance when on, removed again when off). Without it the GPU drops to
// low clocks under FrameLift's light, bursty load and the optical flow engine slows ~2x.
// Returns false (with a reason) on non-NVIDIA systems or if the driver refuses.
bool NvSetMaxPerformance(bool on, std::string& result);
