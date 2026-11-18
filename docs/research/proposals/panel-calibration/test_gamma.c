#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../xnu/files/iokit/Kernel/MiniIOMobileFramebufferCalibration.h"
int main(void)
{
	assert(fbCalRow(74, 0) == 73 && fbCalRow(74, 73) == 0);
	assert(fbCalRow(90, 0) == 89 && fbCalRow(90, 73) == 16);
	assert(34 + fbCalRow(74, 0) * 34 == 2516);
	assert(2550 + fbCalRow(90, 0) * 2 == 2728);
	uint8_t lo[34] = {0}, hi[34] = {0}, out[34];
	/* RGB 255 -> 257 crosses packed high bits. The midpoint is RGB 256,
	 * encoded as 0x15,0,0,0; interpolating the raw bytes would be wrong. */
	lo[1] = lo[2] = lo[3] = 255;
	hi[0] = 0x15; hi[1] = hi[2] = hi[3] = 1;
	lo[4] = 10; hi[4] = 20;
	lo[31] = 0x0f; hi[31] = 0xf0;
	fbCalGamma(lo, hi, 32768, out);
	assert(out[0] == 0x15 && out[1] == 0 && out[2] == 0 && out[3] == 0);
	assert(out[4] == 15 && out[31] == 0x88);
	fbCalGamma(lo, hi, 0, out); assert(!memcmp(out, lo, 34));
	fbCalGamma(lo, hi, 65536, out); assert(!memcmp(out, hi, 34));
	assert(fbCalLerp(2661, 484, 32768) == 1573);
	puts("PASS: reversed 74/90-row endpoints, gamma 10-bit carry, byte/nibble interpolation, endpoints, descending AOR");
	return 0;
}
