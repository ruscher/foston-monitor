#include "status.h"
#include <string.h>
#include <time.h>

static const bf_status_provider_t *prov = &bf_status_sim;
static bf_status_t cache;
static time_t last;

void status_select(const bf_status_provider_t *p) { prov = p; last = 0; }
const bf_status_provider_t *status_provider(void) { return prov; }

void status_refresh(void)
{
    memset(&cache, 0, sizeof cache);
    prov->read(&cache);
    cache.simulated = prov->simulated;
    last = time(NULL);
}

const bf_status_t *status_get(void)
{
    if(time(NULL) != last) status_refresh();
    return &cache;
}
