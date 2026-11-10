/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the Transit app's Providers, by id. See transit_provider.h.
 */
#include "transit/transit_provider.h"

#include <string.h>

#define TRANSIT_LIST_PROVIDER(city) &transit_provider_##city,
static const transit_provider_t *const s_providers[] = {
    TRANSIT_PROVIDERS(TRANSIT_LIST_PROVIDER)
};
#undef TRANSIT_LIST_PROVIDER

const transit_provider_t *transit_provider_find(const char *id)
{
    if (!id || !id[0]) return s_providers[0];
    for (size_t i = 0; i < sizeof s_providers / sizeof s_providers[0]; i++) {
        if (strcmp(s_providers[i]->id, id) == 0) return s_providers[i];
    }
    return NULL;
}
