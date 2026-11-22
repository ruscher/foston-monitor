/* bgkinds.h - minimum display zoom of each feature kind (bgmap_format.h), shared by the
 * PC builder (which levels get the feature) and the device renderer (when it is drawn).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BGKINDS_H
#define BGKINDS_H
#include "bgmap_format.h"

static inline int bgk_minzoom(int k)
{
    switch (k) {
    case BGK_A_RESIDENTIAL: return 10;
    case BGK_A_COMMERCIAL: case BGK_A_INDUSTRIAL: case BGK_A_FARM: return 12;
    case BGK_A_FOREST: return 9;
    case BGK_A_PARK: case BGK_A_SAND: return 11;
    case BGK_A_WETLAND: case BGK_A_AEROWAY: return 10;
    case BGK_A_WATER: return 5;
    case BGK_A_PEDESTRIAN: case BGK_A_PARKING: case BGK_A_BUILDING: return 14;
    case BGK_L_STREAM: case BGK_L_TRACK: return 13;
    case BGK_L_RIVER: case BGK_L_SECONDARY: case BGK_L_FERRY: return 9;
    case BGK_L_BOUNDARY: case BGK_L_TRUNK: case BGK_L_MOTORWAY: return 5;
    case BGK_L_RAIL: case BGK_L_TERTIARY: return 11;
    case BGK_L_PATH: case BGK_L_SERVICE: return 14;
    case BGK_L_RESIDENTIAL: return 12;
    case BGK_L_PRIMARY: return 8;
    case BGK_P_CITY: return 5;
    case BGK_P_TOWN: return 9;
    case BGK_P_SUBURB: case BGK_P_VILLAGE: return 11;
    case BGK_P_NEIGHBOURHOOD: return 14;
    }
    return 99;
}

#endif
