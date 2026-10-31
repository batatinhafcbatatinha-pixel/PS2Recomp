// The standalone GS tests do not run the VU interpreter that owns this
// optional diagnostic history in the full runtime.
#include <cstdint>
uint16_t g_vu1PcRing[4096]{};
uint32_t g_vu1PcRingNext = 0;
