#ifdef TARGET_ABI_MIPSO32  /* o32 */
#define TARGET_SYSCALL_OFFSET 4000
#include "syscall_o32_nr.h"
#elif defined TARGET_ABI_MIPSP32 /* p32 */
#define TARGET_SYSCALL_OFFSET 0
#include "syscall_p32_nr.h"
#endif
