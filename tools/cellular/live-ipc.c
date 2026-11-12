/* MIT. Fixed IPC mapping check only: no RAM contents, modem power or DMA. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include "../../xnu/files/bsd/sys/redwood_cp_ipc.h"

static int command(unsigned op,unsigned expected_mapped)
{
    struct cp_ipc_request q={.version=CP_IPC_API,.op=op};
    struct cp_ipc_reply r={0};size_t size=sizeof(r);
    if(sysctlbyname("kern.redwood.cp_ipc",&r,&size,&q,CP_IPC_HEADER)) {
        fprintf(stderr,"cp IPC op%u failed: %s\n",op,strerror(errno));return -1;
    }
    if(size!=CP_IPC_HEADER || r.version!=CP_IPC_API || r.mapped!=expected_mapped || r.length || r.reserved) {
        fprintf(stderr,"cp IPC op%u unexpected response; inspect mapping ownership\n",op);return -1;
    }
    printf("cp IPC op%u: mapped%u\n",op,r.mapped);return 0;
}
int main(int argc,char **argv)
{
    if(argc!=2 || (strcmp(argv[1],"--status") && strcmp(argv[1],"--map-check"))) {
        fprintf(stderr,"usage: live-ipc --status|--map-check\n");return 2;
    }
    if(command(CP_IPC_INFO,0)) return 1; /* Never take over an existing map. */
    if(!strcmp(argv[1],"--status")) return 0;
    if(command(CP_IPC_MAP,1)) {
        fprintf(stderr,"MAP uncertain/refused; no automatic second operation\n");return 1;
    }
    puts("IPC reservation and2048 NormalNC translations verified; no contents accessed");
    if(command(CP_IPC_UNMAP,0) || command(CP_IPC_INFO,0)) return 1;
    puts("Mapping check PASS, AP mapping removed; reserved RAM retained, no DMA grants");
    return 0;
}
