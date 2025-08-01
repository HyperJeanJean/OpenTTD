/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file road.h Road specific functions. */

#ifndef ROAD_H
#define ROAD_H

#include "road_type.h"
#include "gfx_type.h"
#include "core/bitmath_func.hpp"
#include "strings_type.h"
#include "timer/timer_game_calendar.h"
#include "core/enum_type.hpp"
#include "newgrf.h"
#include "newgrf_badge_type.h"
#include "economy_func.h"


enum RoadTramType : bool {
	RTT_ROAD,
	RTT_TRAM,
};

enum RoadTramTypes : uint8_t {
	RTTB_ROAD = 1 << RTT_ROAD,
	RTTB_TRAM = 1 << RTT_TRAM,
};
DECLARE_ENUM_AS_BIT_SET(RoadTramTypes)

static const RoadTramType _roadtramtypes[] = { RTT_ROAD, RTT_TRAM };

/** Roadtype flag bit numbers. */
enum class RoadTypeFlag : uint8_t {
	Catenary        = 0, ///< Bit number for adding catenary
	NoLevelCrossing = 1, ///< Bit number for disabling level crossing
	NoHouses        = 2, ///< Bit number for setting this roadtype as not house friendly
	Hidden          = 3, ///< Bit number for hidden from construction.
	TownBuild       = 4, ///< Bit number for allowing towns to build this roadtype.
};
using RoadTypeFlags = EnumBitSet<RoadTypeFlag, uint8_t>;

struct SpriteGroup;

/** Sprite groups for a roadtype. */
enum RoadTypeSpriteGroup : uint8_t {
	ROTSG_CURSORS,        ///< Optional: Cursor and toolbar icon images
	ROTSG_OVERLAY,        ///< Optional: Images for overlaying track
	ROTSG_GROUND,         ///< Required: Main group of ground images
	ROTSG_TUNNEL,         ///< Optional: Ground images for tunnels
	ROTSG_CATENARY_FRONT, ///< Optional: Catenary front
	ROTSG_CATENARY_BACK,  ///< Optional: Catenary back
	ROTSG_BRIDGE,         ///< Required: Bridge surface images
	ROTSG_reserved2,      ///<           Placeholder, if we need specific level crossing sprites.
	ROTSG_DEPOT,          ///< Optional: Depot images
	ROTSG_reserved3,      ///<           Placeholder, if we add road fences (for highways).
	ROTSG_ROADSTOP,       ///< Required: Bay stop surface
	ROTSG_ONEWAY,         ///< Optional: One-way indicator images
	ROTSG_END,
};

/** List of road type labels. */
typedef std::vector<RoadTypeLabel> RoadTypeLabelList;

class RoadTypeInfo {
public:
	/**
	 * struct containing the sprites for the road GUI. @note only sprites referred to
	 * directly in the code are listed
	 */
	struct {
		SpriteID build_x_road;        ///< button for building single rail in X direction
		SpriteID build_y_road;        ///< button for building single rail in Y direction
		SpriteID auto_road;           ///< button for the autoroad construction
		SpriteID build_depot;         ///< button for building depots
		SpriteID build_tunnel;        ///< button for building a tunnel
		SpriteID convert_road;        ///< button for converting road types
	} gui_sprites;

	struct {
		CursorID road_swne;     ///< Cursor for building rail in X direction
		CursorID road_nwse;     ///< Cursor for building rail in Y direction
		CursorID autoroad;      ///< Cursor for autorail tool
		CursorID depot;         ///< Cursor for building a depot
		CursorID tunnel;        ///< Cursor for building a tunnel
		SpriteID convert_road;  ///< Cursor for converting road types
	} cursor;                       ///< Cursors associated with the road type.

	struct {
		StringID name;            ///< Name of this rail type.
		StringID toolbar_caption; ///< Caption in the construction toolbar GUI for this rail type.
		StringID menu_text;       ///< Name of this rail type in the main toolbar dropdown.
		StringID build_caption;   ///< Caption of the build vehicle GUI for this rail type.
		StringID replace_text;    ///< Text used in the autoreplace GUI.
		StringID new_engine;      ///< Name of an engine for this type of road in the engine preview GUI.

		StringID err_build_road;        ///< Building a normal piece of road
		StringID err_remove_road;       ///< Removing a normal piece of road
		StringID err_depot;             ///< Building a depot
		StringID err_build_station[2];  ///< Building a bus or truck station
		StringID err_remove_station[2]; ///< Removing of a bus or truck station
		StringID err_convert_road;      ///< Converting a road type

		StringID picker_title[2];       ///< Title for the station picker for bus or truck stations
		StringID picker_tooltip[2];     ///< Tooltip for the station picker for bus or truck stations
	} strings;                        ///< Strings associated with the rail type.

	/** bitmask to the OTHER roadtypes on which a vehicle of THIS roadtype generates power */
	RoadTypes powered_roadtypes;

	/**
	 * Bit mask of road type flags
	 */
	RoadTypeFlags flags;

	/**
	 * Cost multiplier for building this road type
	 */
	uint16_t cost_multiplier;

	/**
	 * Cost multiplier for maintenance of this road type
	 */
	uint16_t maintenance_multiplier;

	/**
	 * Maximum speed for vehicles travelling on this road type
	 */
	uint16_t max_speed;

	/**
	 * Unique 32 bit road type identifier
	 */
	RoadTypeLabel label;

	/**
	 * Road type labels this type provides in addition to the main label.
	 */
	RoadTypeLabelList alternate_labels;

	/**
	 * Colour on mini-map
	 */
	PixelColour map_colour;

	/**
	 * Introduction date.
	 * When #INVALID_DATE or a vehicle using this roadtype gets introduced earlier,
	 * the vehicle's introduction date will be used instead for this roadtype.
	 * The introduction at this date is furthermore limited by the
	 * #introduction_required_types.
	 */
	TimerGameCalendar::Date introduction_date;

	/**
	 * Bitmask of roadtypes that are required for this roadtype to be introduced
	 * at a given #introduction_date.
	 */
	RoadTypes introduction_required_roadtypes;

	/**
	 * Bitmask of which other roadtypes are introduced when this roadtype is introduced.
	 */
	RoadTypes introduces_roadtypes;

	/**
	 * The sorting order of this roadtype for the toolbar dropdown.
	 */
	uint8_t sorting_order;

	/**
	 * NewGRF providing the Action3 for the roadtype. nullptr if not available.
	 */
	const GRFFile *grffile[ROTSG_END];

	/**
	 * Sprite groups for resolving sprites
	 */
	const SpriteGroup *group[ROTSG_END];

	std::vector<BadgeID> badges;

	inline bool UsesOverlay() const
	{
		return this->group[ROTSG_GROUND] != nullptr;
	}
};

/**
 * Get the mask for road types of the given RoadTramType.
 * @param rtt RoadTramType.
 * @return Mask of road types for RoadTramType.
 */
inline RoadTypes GetMaskForRoadTramType(RoadTramType rtt)
{
	extern RoadTypes _roadtypes_road;
	extern RoadTypes _roadtypes_tram;
	return rtt == RTT_ROAD ? _roadtypes_road : _roadtypes_tram;
}

inline bool RoadTypeIsRoad(RoadType roadtype)
{
	return GetMaskForRoadTramType(RTT_ROAD).Test(roadtype);
}

inline bool RoadTypeIsTram(RoadType roadtype)
{
	return GetMaskForRoadTramType(RTT_TRAM).Test(roadtype);
}

inline RoadTramType GetRoadTramType(RoadType roadtype)
{
	return RoadTypeIsTram(roadtype) ? RTT_TRAM : RTT_ROAD;
}

inline RoadTramType OtherRoadTramType(RoadTramType rtt)
{
	return rtt == RTT_ROAD ? RTT_TRAM : RTT_ROAD;
}

/**
 * Returns a pointer to the Roadtype information for a given roadtype
 * @param roadtype the road type which the information is requested for
 * @return The pointer to the RoadTypeInfo
 */
inline const RoadTypeInfo *GetRoadTypeInfo(RoadType roadtype)
{
	extern RoadTypeInfo _roadtypes[ROADTYPE_END];
	assert(roadtype < ROADTYPE_END);
	return &_roadtypes[roadtype];
}

/**
 * Returns the railtype for a Railtype information.
 * @param rti Pointer to static RailTypeInfo
 * @return Railtype in static railtype definitions
 */
inline RoadType GetRoadTypeInfoIndex(const RoadTypeInfo *rti)
{
	extern RoadTypeInfo _roadtypes[ROADTYPE_END];
	size_t index = rti - _roadtypes;
	assert(index < ROADTYPE_END && rti == _roadtypes + index);
	return static_cast<RoadType>(index);
}

/**
 * Checks if an engine of the given RoadType got power on a tile with a given
 * RoadType. This would normally just be an equality check, but for electrified
 * roads (which also support non-electric vehicles).
 * @param  enginetype The RoadType of the engine we are considering.
 * @param  tiletype   The RoadType of the tile we are considering.
 * @return Whether the engine got power on this tile.
 */
inline bool HasPowerOnRoad(RoadType enginetype, RoadType tiletype)
{
	return GetRoadTypeInfo(enginetype)->powered_roadtypes.Test(tiletype);
}

/**
 * Returns the cost of building the specified roadtype.
 * @param roadtype The roadtype being built.
 * @return The cost multiplier.
 */
inline Money RoadBuildCost(RoadType roadtype)
{
	assert(roadtype < ROADTYPE_END);
	return (_price[PR_BUILD_ROAD] * GetRoadTypeInfo(roadtype)->cost_multiplier) >> 3;
}

/**
 * Returns the cost of clearing the specified roadtype.
 * @param roadtype The roadtype being removed.
 * @return The cost.
 */
inline Money RoadClearCost(RoadType roadtype)
{
	assert(roadtype < ROADTYPE_END);

	/* Flat fee for removing road. */
	if (RoadTypeIsRoad(roadtype)) return _price[PR_CLEAR_ROAD];

	/* Clearing tram earns a little money, but also incurs the standard clear road cost,
	 * so no profit can be made. */
	return _price[PR_CLEAR_ROAD] - RoadBuildCost(roadtype) * 3 / 4;
}

/**
 * Calculates the cost of road conversion
 * @param from The roadtype we are converting from
 * @param to   The roadtype we are converting to
 * @return Cost per RoadBit
 */
inline Money RoadConvertCost(RoadType from, RoadType to)
{
	/* Don't apply convert costs when converting to the same roadtype (ex. building a roadstop over existing road) */
	if (from == to) return (Money)0;

	/* Same cost as removing and then building. */
	return RoadBuildCost(to) + RoadClearCost(from);
}

/**
 * Test if road disallows level crossings
 * @param roadtype The roadtype we are testing
 * @return True iff the roadtype disallows level crossings
 */
inline bool RoadNoLevelCrossing(RoadType roadtype)
{
	assert(roadtype < ROADTYPE_END);
	return GetRoadTypeInfo(roadtype)->flags.Test(RoadTypeFlag::NoLevelCrossing);
}

RoadType GetRoadTypeByLabel(RoadTypeLabel label, bool allow_alternate_labels = true);

void ResetRoadTypes();
void InitRoadTypes();
RoadType AllocateRoadType(RoadTypeLabel label, RoadTramType rtt);
bool HasAnyRoadTypesAvail(CompanyID company, RoadTramType rtt);

extern std::vector<RoadType> _sorted_roadtypes;
extern RoadTypes _roadtypes_hidden_mask;

#endif /* ROAD_H */
