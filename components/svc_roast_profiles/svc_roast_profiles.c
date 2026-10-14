#include <stddef.h>
#include "svc_roast_profiles.h"

static const roast_profile_t PROFILES[] = {
    { "Claro", 120.0f, 3,
      { { "Secado", 150.0f, 240 }, { "Maillard", 185.0f, 180 }, { "Desarrollo", 196.0f, 100 } } },
    { "Medio-claro", 120.0f, 3,
      { { "Secado", 150.0f, 240 }, { "Maillard", 185.0f, 180 }, { "Desarrollo", 205.0f, 130 } } },
    { "Medio", 120.0f, 3,
      { { "Secado", 150.0f, 240 }, { "Maillard", 185.0f, 180 }, { "Desarrollo", 212.0f, 160 } } },
    { "Medio-oscuro", 120.0f, 3,
      { { "Secado", 150.0f, 240 }, { "Maillard", 185.0f, 180 }, { "Desarrollo", 220.0f, 190 } } },
    { "Oscuro", 120.0f, 3,
      { { "Secado", 150.0f, 240 }, { "Maillard", 185.0f, 180 }, { "Desarrollo", 228.0f, 220 } } },
};

#define PROFILE_COUNT (sizeof(PROFILES) / sizeof(PROFILES[0]))
_Static_assert(PROFILE_COUNT <= ROAST_PROFILE_MAX, "demasiados perfiles");

uint8_t svc_roast_profiles_count(void)
{
    return (uint8_t)PROFILE_COUNT;
}

const roast_profile_t *svc_roast_profiles_get(roast_profile_id_t id)
{
    return (id < PROFILE_COUNT) ? &PROFILES[id] : NULL;
}

uint16_t svc_roast_profiles_total_s(const roast_profile_t *p)
{
    uint16_t total = 0;
    if (p) {
        for (uint8_t i = 0; i < p->stage_count; i++) {
            total += p->stages[i].duration_s;
        }
    }
    return total;
}
