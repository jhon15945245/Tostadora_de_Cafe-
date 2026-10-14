#pragma once

#include <stdint.h>
#include "roast_types.h"

#define ROAST_PROFILE_MAX         8
#define ROAST_PROFILE_MAX_STAGES  3

typedef struct {
    const char *name;
    float       target_c;
    uint16_t    duration_s;
} roast_stage_t;

typedef struct {
    const char   *name;
    float         charge_c;
    uint8_t       stage_count;
    roast_stage_t stages[ROAST_PROFILE_MAX_STAGES];
} roast_profile_t;

uint8_t                svc_roast_profiles_count(void);
const roast_profile_t *svc_roast_profiles_get(roast_profile_id_t id);
uint16_t               svc_roast_profiles_total_s(const roast_profile_t *p);
