// libSceNet initialization (required for UDP on Orbis).
#include "net_orbis.h"
#include "log.h"

#ifdef __ORBIS__
#include <stddef.h>
#include <orbis/Net.h>
#include <orbis/Sysmodule.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <errno.h>

#define NET_POOL_SIZE (256 * 1024)

static int s_pool_id = -1;

int net_orbis_init(void) {
    int ret;

    ret = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
    LOGI("net: LoadModuleInternal(NET) => 0x%08x", (unsigned)ret);

    ret = sceNetInit();
    LOGI("net: sceNetInit => 0x%08x", (unsigned)ret);

    if (s_pool_id < 0) {
        ret = sceNetPoolCreate("moonlight", NET_POOL_SIZE, 0);
        LOGI("net: sceNetPoolCreate => 0x%08x", (unsigned)ret);
        if (ret < 0) {
            LOGE("net: pool FAIL");
            return -1;
        }
        s_pool_id = ret;
    }

    {
        int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd < 0) {
            LOGW("net: socket(UDP,17) errno=%d; retry protocol=0", errno);
            fd = socket(AF_INET, SOCK_DGRAM, 0);
        }
        if (fd < 0) {
            LOGE("net: socket UDP FAIL errno=%d", errno);
            return -1;
        }
        LOGI("net: socket UDP OK fd=%d", fd);
        close(fd);
    }
    return 0;
}

void net_orbis_shutdown(void) {
    if (s_pool_id >= 0) {
        sceNetPoolDestroy(s_pool_id);
        s_pool_id = -1;
    }
}
#else
int net_orbis_init(void) { return 0; }
void net_orbis_shutdown(void) {}
#endif
