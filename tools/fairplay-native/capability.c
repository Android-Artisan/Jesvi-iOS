/* Provider presence probe, not a decryption test. No file or account access.
 * In this XNU, cryptid 1/2 checks text_crypter_create before vnode lookup.
 * Address zero therefore returns ENOTSUP with no provider, EINVAL with one.
 * cryptid 0 is a control: the implemented syscall returns success immediately.
 */
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

extern int mremap_encrypted(void *, size_t, uint32_t, uint32_t, uint32_t);

int main(void)
{
    const unsigned ids[] = {0, 1, 2};
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        errno = 0;
        int result = mremap_encrypted(NULL, (size_t)getpagesize(), ids[i],
                                     0x0100000c, 0);
        printf("cryptid=%u result=%d errno=%d\n", ids[i], result, errno);
    }
    return 0;
}
