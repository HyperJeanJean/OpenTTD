/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file object_cmd.cpp Handling of object tiles. */

#include "stdafx.h"
#include "landscape.h"
#include "command_func.h"
#include "company_func.h"
#include "viewport_func.h"
#include "company_base.h"
#include "town.h"
#include "bridge_map.h"
#include "genworld.h"
#include "autoslope.h"
#include "clear_func.h"
#include "water.h"
#include "window_func.h"
#include "company_gui.h"
#include "cheat_type.h"
#include "object.h"
#include "cargopacket.h"
#include "core/random_func.hpp"
#include "core/pool_func.hpp"
#include "object_map.h"
#include "object_base.h"
#include "newgrf_config.h"
#include "newgrf_object.h"
#include "timer/timer_game_calendar.h"
#include "newgrf_debug.h"
#include "vehicle_func.h"
#include "station_func.h"
#include "object_cmd.h"
#include "landscape_cmd.h"
#include "pathfinder/water_regions.h"

#include "table/strings.h"
#include "table/object_land.h"

#include "safeguards.h"

ObjectPool _object_pool("Object");
INSTANTIATE_POOL_METHODS(Object)
/* static */ std::array<uint16_t, NUM_OBJECTS> Object::counts;

/**
 * Get the object associated with a tile.
 * @param tile The tile to fetch the object for.
 * @return The object.
 */
/* static */ Object *Object::GetByTile(TileIndex tile)
{
	return Object::Get(GetObjectIndex(tile));
}

/**
 * Gets the ObjectType of the given object tile
 * @param t the tile to get the type from.
 * @pre IsTileType(t, MP_OBJECT)
 * @return the type.
 */
ObjectType GetObjectType(Tile t)
{
	assert(IsTileType(t, MP_OBJECT));
	return Object::GetByTile(t)->type;
}

/** Initialize/reset the objects. */
void InitializeObjects()
{
	Object::ResetTypeCounts();
}

/**
 * Actually build the object.
 * @param type  The type of object to build.
 * @param tile  The tile to build the northern tile of the object on.
 * @param owner The owner of the object.
 * @param town  Town the tile is related with.
 * @param view  The view for the object.
 * @pre All preconditions for building the object at that location
 *      are met, e.g. slope and clearness of tiles are checked.
 */
void BuildObject(ObjectType type, TileIndex tile, CompanyID owner, Town *town, uint8_t view)
{
	const ObjectSpec *spec = ObjectSpec::Get(type);

	TileArea ta(tile, GB(spec->size, HasBit(view, 0) ? 4 : 0, 4), GB(spec->size, HasBit(view, 0) ? 0 : 4, 4));
	Object *o = new Object(type, town == nullptr ? CalcClosestTownFromTile(tile) : town, ta, TimerGameCalendar::date, view);

	/* If nothing owns the object, the colour will be random. Otherwise
	 * get the colour from the company's livery settings. */
	if (owner == OWNER_NONE) {
		o->colour = Random();
	} else {
		const Livery &l = Company::Get(owner)->livery[0];
		o->colour = l.colour1 + l.colour2 * 16;
	}

	/* If the object wants only one colour, then give it that colour. */
	if (!spec->flags.Test(ObjectFlag::Uses2CC)) o->colour &= 0xF;

	if (spec->callback_mask.Test(ObjectCallbackMask::Colour)) {
		uint16_t res = GetObjectCallback(CBID_OBJECT_COLOUR, o->colour, 0, spec, o, tile);
		if (res != CALLBACK_FAILED) {
			if (res >= 0x100) ErrorUnknownCallbackResult(spec->grf_prop.grfid, CBID_OBJECT_COLOUR, res);
			o->colour = GB(res, 0, 8);
		}
	}

	assert(o->town != nullptr);

	for (TileIndex t : ta) {
		if (IsWaterTile(t)) ClearNeighbourNonFloodingStates(t);
		if (HasTileWaterGround(t)) InvalidateWaterRegion(t);
		WaterClass wc = (IsWaterTile(t) ? GetWaterClass(t) : WATER_CLASS_INVALID);
		/* Update company infrastructure counts for objects build on canals owned by nobody. */
		if (wc == WATER_CLASS_CANAL && owner != OWNER_NONE && (IsTileOwner(t, OWNER_NONE) || IsTileOwner(t, OWNER_WATER))) {
			Company::Get(owner)->infrastructure.water++;
			DirtyCompanyInfrastructureWindows(owner);
		}
		bool remove = IsDockingTile(t);
		MakeObject(t, owner, o->index, wc, Random());
		if (remove) RemoveDockingTile(t);
		MarkTileDirtyByTile(t);
	}

	Object::IncTypeCount(type);
	if (spec->flags.Test(ObjectFlag::Animation)) TriggerObjectAnimation(o, ObjectAnimationTrigger::Built, spec);
}

/**
 * Increase the HQ size.
 * @param tile The (northern) tile of the company HQ.
 */
static void IncreaseCompanyHQSize(TileIndex tile)
{
	TileArea ta = Object::GetByTile(tile)->location;
	for (TileIndex t : ta) {
		/* We encode the company HQ size in the animation state. */
		SetAnimationFrame(t, GetAnimationFrame(t) + 1);
		MarkTileDirtyByTile(t);
	}
}

/**
 * Get the size of the HQ.
 * @param tile The (northern) tile of the company HQ.
 * @return HQ size.
 */
static uint8_t GetCompanyHQSize(TileIndex tile)
{
	/* We encode the company HQ size in the animation state. */
	return GetAnimationFrame(tile);
}

/**
 * Update the CompanyHQ to the state associated with the given score
 * @param tile  The (northern) tile of the company HQ, or INVALID_TILE.
 * @param score The current (performance) score of the company.
 */
void UpdateCompanyHQ(TileIndex tile, uint score)
{
	if (tile == INVALID_TILE) return;

	uint8_t val = 0;
	if (score >= 170) val++;
	if (score >= 350) val++;
	if (score >= 520) val++;
	if (score >= 720) val++;

	while (GetCompanyHQSize(tile) < val) {
		IncreaseCompanyHQSize(tile);
	}
}

/**
 * Updates the colour of the object whenever a company changes.
 * @param c The company the company colour changed of.
 */
void UpdateObjectColours(const Company *c)
{
	for (Object *obj : Object::Iterate()) {
		Owner owner = GetTileOwner(obj->location.tile);
		/* Not the current owner, so colour doesn't change. */
		if (owner != c->index) continue;

		const ObjectSpec *spec = ObjectSpec::GetByTile(obj->location.tile);
		/* Using the object colour callback, so not using company colour. */
		if (spec->callback_mask.Test(ObjectCallbackMask::Colour)) continue;

		const Livery &l = c->livery[0];
		obj->colour = (spec->flags.Test(ObjectFlag::Uses2CC) ? (l.colour2 * 16) : 0) + l.colour1;
	}
}

extern CommandCost CheckBuildableTile(TileIndex tile, DiagDirections invalid_dirs, int &allowed_z, bool allow_steep, bool check_bridge);
static CommandCost ClearTile_Object(TileIndex tile, DoCommandFlags flags);

/**
 * Build an object object
 * @param flags type of operation
 * @param tile tile where the object will be located
 * @param type the object type to build
 * @param view the view for the object
 * @return the cost of this operation or an error
 */
CommandCost CmdBuildObject(DoCommandFlags flags, TileIndex tile, ObjectType type, uint8_t view)
{
	CommandCost cost(EXPENSES_CONSTRUCTION);

	if (type >= ObjectSpec::Count()) return CMD_ERROR;
	const ObjectSpec *spec = ObjectSpec::Get(type);
	if (_game_mode == GM_NORMAL && !spec->IsAvailable() && !_generating_world) return CMD_ERROR;
	if ((_game_mode == GM_EDITOR || _generating_world) && !spec->WasEverAvailable()) return CMD_ERROR;

	if (spec->flags.Test(ObjectFlag::OnlyInScenedit) && ((!_generating_world && _game_mode != GM_EDITOR) || _current_company != OWNER_NONE)) return CMD_ERROR;
	if (spec->flags.Test(ObjectFlag::OnlyInGame) && (_generating_world || _game_mode != GM_NORMAL || _current_company > MAX_COMPANIES)) return CMD_ERROR;
	if (view >= spec->views) return CMD_ERROR;

	if (!Object::CanAllocateItem()) return CommandCost(STR_ERROR_TOO_MANY_OBJECTS);
	if (Town::GetNumItems() == 0) return CommandCost(STR_ERROR_MUST_FOUND_TOWN_FIRST);

	int size_x = GB(spec->size, HasBit(view, 0) ? 4 : 0, 4);
	int size_y = GB(spec->size, HasBit(view, 0) ? 0 : 4, 4);
	TileArea ta(tile, size_x, size_y);
	for (TileIndex t : ta) {
		if (!IsValidTile(t)) return CommandCost(STR_ERROR_TOO_CLOSE_TO_EDGE_OF_MAP_SUB); // Might be off the map
	}

	if (type == OBJECT_OWNED_LAND) {
		/* Owned land is special as it can be placed on any slope. */
		cost.AddCost(Command<CMD_LANDSCAPE_CLEAR>::Do(flags, tile));
	} else {
		/* Check the surface to build on. At this time we can't actually execute the
		 * the CLEAR_TILE commands since the newgrf callback later on can check
		 * some information about the tiles. */
		bool allow_water = spec->flags.Any({ObjectFlag::BuiltOnWater, ObjectFlag::NotOnLand});
		bool allow_ground = !spec->flags.Test(ObjectFlag::NotOnLand);
		for (TileIndex t : ta) {
			if (HasTileWaterGround(t)) {
				if (!allow_water) return CommandCost(STR_ERROR_CAN_T_BUILD_ON_WATER);
				if (!IsWaterTile(t)) {
					/* Normal water tiles don't have to be cleared. For all other tile types clear
					 * the tile but leave the water. */
					cost.AddCost(Command<CMD_LANDSCAPE_CLEAR>::Do(DoCommandFlags{flags}.Reset({DoCommandFlag::NoWater, DoCommandFlag::Execute}), t));
				} else {
					/* Can't build on water owned by another company. */
					Owner o = GetTileOwner(t);
					if (o != OWNER_NONE && o != OWNER_WATER) cost.AddCost(CheckOwnership(o, t));

					/* However, the tile has to be clear of vehicles. */
					cost.AddCost(EnsureNoVehicleOnGround(t));
				}
			} else {
				if (!allow_ground) return CommandCost(STR_ERROR_MUST_BE_BUILT_ON_WATER);
				/* For non-water tiles, we'll have to clear it before building. */

				/* When relocating HQ, allow it to be relocated (partial) on itself. */
				if (!(type == OBJECT_HQ &&
						IsTileType(t, MP_OBJECT) &&
						IsTileOwner(t, _current_company) &&
						IsObjectType(t, OBJECT_HQ))) {
					cost.AddCost(Command<CMD_LANDSCAPE_CLEAR>::Do(DoCommandFlags{flags}.Reset(DoCommandFlag::Execute), t));
				}
			}
		}

		/* So, now the surface is checked... check the slope of said surface. */
		auto [slope, allowed_z] = GetTileSlopeZ(tile);
		if (slope != SLOPE_FLAT) allowed_z++;

		for (TileIndex t : ta) {
			uint16_t callback = CALLBACK_FAILED;
			std::array<int32_t, 16> regs100;
			if (spec->callback_mask.Test(ObjectCallbackMask::SlopeCheck)) {
				TileIndex diff = t - tile;
				callback = GetObjectCallback(CBID_OBJECT_LAND_SLOPE_CHECK, GetTileSlope(t), TileY(diff) << 4 | TileX(diff), spec, nullptr, t, regs100, view);
			}

			if (callback == CALLBACK_FAILED) {
				cost.AddCost(CheckBuildableTile(t, {}, allowed_z, false, false));
			} else {
				/* The meaning of bit 10 is inverted for a grf version < 8. */
				if (spec->grf_prop.grffile->grf_version < 8) ToggleBit(callback, 10);
				CommandCost ret = GetErrorMessageFromLocationCallbackResult(callback, regs100, spec->grf_prop.grffile, STR_ERROR_LAND_SLOPED_IN_WRONG_DIRECTION);
				if (ret.Failed()) return ret;
			}
		}

		if (flags.Test(DoCommandFlag::Execute)) {
			/* This is basically a copy of the loop above with the exception that we now
			 * execute the commands and don't check for errors, since that's already done. */
			for (TileIndex t : ta) {
				if (HasTileWaterGround(t)) {
					if (!IsWaterTile(t)) {
						Command<CMD_LANDSCAPE_CLEAR>::Do(DoCommandFlags{flags}.Reset(DoCommandFlag::NoWater).Set(DoCommandFlag::NoModifyTownRating), t);
					}
				} else {
					Command<CMD_LANDSCAPE_CLEAR>::Do(flags | DoCommandFlag::NoModifyTownRating, t);
				}
			}
		}
	}
	if (cost.Failed()) return cost;

	/* Finally do a check for bridges. */
	for (TileIndex t : ta) {
		if (IsBridgeAbove(t) && (
				!spec->flags.Test(ObjectFlag::AllowUnderBridge) ||
				(GetTileMaxZ(t) + spec->height >= GetBridgeHeight(GetSouthernBridgeEnd(t))))) {
			return CommandCost(STR_ERROR_MUST_DEMOLISH_BRIDGE_FIRST);
		}
	}

	int hq_score = 0;
	uint build_object_size = 1;
	switch (type) {
		case OBJECT_TRANSMITTER:
		case OBJECT_LIGHTHOUSE:
			if (!IsTileFlat(tile)) return CommandCost(STR_ERROR_FLAT_LAND_REQUIRED);
			break;

		case OBJECT_OWNED_LAND:
			if (IsTileType(tile, MP_OBJECT) &&
					IsTileOwner(tile, _current_company) &&
					IsObjectType(tile, OBJECT_OWNED_LAND)) {
				return CommandCost(STR_ERROR_YOU_ALREADY_OWN_IT);
			}
			break;

		case OBJECT_HQ: {
			Company *c = Company::Get(_current_company);
			if (c->location_of_HQ != INVALID_TILE) {
				/* Don't relocate HQ on the same location. */
				if (c->location_of_HQ == tile) return CommandCost(STR_ERROR_ALREADY_BUILT);
				/* We need to persuade a bit harder to remove the old HQ. */
				_current_company = OWNER_WATER;
				cost.AddCost(ClearTile_Object(c->location_of_HQ, flags));
				_current_company = c->index;
			}

			if (flags.Test(DoCommandFlag::Execute)) {
				hq_score = UpdateCompanyRatingAndValue(c, false);
				c->location_of_HQ = tile;
				SetWindowDirty(WC_COMPANY, c->index);
			}
			break;
		}

		case OBJECT_STATUE:
			/* This may never be constructed using this method. */
			return CMD_ERROR;

		default: // i.e. NewGRF provided.
			build_object_size = size_x * size_y;
			break;
	}

	/* Don't allow building more objects if the company has reached its limit. */
	Company *c = Company::GetIfValid(_current_company);
	if (c != nullptr && GB(c->build_object_limit, 16, 16) < build_object_size) {
		return CommandCost(STR_ERROR_BUILD_OBJECT_LIMIT_REACHED);
	}

	if (flags.Test(DoCommandFlag::Execute)) {
		BuildObject(type, tile, _current_company == OWNER_DEITY ? OWNER_NONE : _current_company, nullptr, view);

		/* Make sure the HQ starts at the right size. */
		if (type == OBJECT_HQ) UpdateCompanyHQ(tile, hq_score);

		/* Subtract the tile from the build limit. */
		if (c != nullptr) c->build_object_limit -= build_object_size << 16;
	}

	cost.AddCost(spec->GetBuildCost() * build_object_size);
	return cost;
}

/**
 * Construct multiple objects in an area
 * @param flags of operation to conduct
 * @param tile end tile of area dragging
 * @param start_tile start tile of area dragging
 * @param type the object type to build
 * @param view the view for the object
 * @param diagonal Whether to use the Orthogonal (0) or Diagonal (1) iterator.
 * @return the cost of this operation or an error
 */
CommandCost CmdBuildObjectArea(DoCommandFlags flags, TileIndex tile, TileIndex start_tile, ObjectType type, uint8_t view, bool diagonal)
{
	if (start_tile >= Map::Size()) return CMD_ERROR;

	if (type >= ObjectSpec::Count()) return CMD_ERROR;
	const ObjectSpec *spec = ObjectSpec::Get(type);
	if (view >= spec->views) return CMD_ERROR;

	if (spec->size != OBJECT_SIZE_1X1) return CMD_ERROR;

	Money money = GetAvailableMoneyForCommand();
	CommandCost cost(EXPENSES_CONSTRUCTION);
	CommandCost last_error = CMD_ERROR;
	bool had_success = false;

	const Company *c = Company::GetIfValid(_current_company);
	int limit = (c == nullptr ? INT32_MAX : GB(c->build_object_limit, 16, 16));

	std::unique_ptr<TileIterator> iter = TileIterator::Create(tile, start_tile, diagonal);
	for (; *iter != INVALID_TILE; ++(*iter)) {
		TileIndex t = *iter;
		CommandCost ret = Command<CMD_BUILD_OBJECT>::Do(DoCommandFlags{flags}.Reset(DoCommandFlag::Execute), t, type, view);

		/* If we've reached the limit, stop building (or testing). */
		if (c != nullptr && limit-- <= 0) break;

		if (ret.Failed()) {
			last_error = std::move(ret);
			continue;
		}

		had_success = true;
		if (flags.Test(DoCommandFlag::Execute)) {
			money -= ret.GetCost();

			/* If we run out of money, stop building. */
			if (ret.GetCost() > 0 && money < 0) break;
			Command<CMD_BUILD_OBJECT>::Do(flags, t, type, view);
		}
		cost.AddCost(ret.GetCost());
	}

	return had_success ? cost : last_error;
}

static Foundation GetFoundation_Object(TileIndex tile, Slope tileh);

static void DrawTile_Object(TileInfo *ti)
{
	ObjectType type = GetObjectType(ti->tile);
	const ObjectSpec *spec = ObjectSpec::Get(type);

	/* Fall back for when the object doesn't exist anymore. */
	if (!spec->IsEnabled()) type = OBJECT_TRANSMITTER;

	if (!spec->flags.Test(ObjectFlag::HasNoFoundation)) DrawFoundation(ti, GetFoundation_Object(ti->tile, ti->tileh));

	if (type < NEW_OBJECT_OFFSET) {
		const DrawTileSprites *dts = nullptr;
		Owner to = GetTileOwner(ti->tile);
		PaletteID palette = to == OWNER_NONE ? PAL_NONE : GetCompanyPalette(to);

		if (type == OBJECT_HQ) {
			TileIndex diff = ti->tile - Object::GetByTile(ti->tile)->location.tile;
			dts = &_object_hq[GetCompanyHQSize(ti->tile) << 2 | TileY(diff) << 1 | TileX(diff)];
		} else {
			dts = &_objects[type];
		}

		if (spec->flags.Test(ObjectFlag::HasNoFoundation)) {
			/* If an object has no foundation, but tries to draw a (flat) ground
			 * type... we have to be nice and convert that for them. */
			switch (dts->ground.sprite) {
				case SPR_FLAT_BARE_LAND:          DrawClearLandTile(ti, 0); break;
				case SPR_FLAT_1_THIRD_GRASS_TILE: DrawClearLandTile(ti, 1); break;
				case SPR_FLAT_2_THIRD_GRASS_TILE: DrawClearLandTile(ti, 2); break;
				case SPR_FLAT_GRASS_TILE:         DrawClearLandTile(ti, 3); break;
				default: DrawGroundSprite(dts->ground.sprite, palette);     break;
			}
		} else {
			DrawGroundSprite(dts->ground.sprite, palette);
		}

		if (!IsInvisibilitySet(TO_STRUCTURES)) {
			for (const DrawTileSeqStruct &dtss : dts->GetSequence()) {
				AddSortableSpriteToDraw(dtss.image.sprite, palette, *ti, dtss, IsTransparencySet(TO_STRUCTURES));
			}
		}
	} else {
		DrawNewObjectTile(ti, spec);
	}

	DrawBridgeMiddle(ti);
}

static int GetSlopePixelZ_Object(TileIndex tile, uint x, uint y, bool)
{
	if (IsObjectType(tile, OBJECT_OWNED_LAND)) {
		auto [tileh, z] = GetTilePixelSlope(tile);

		return z + GetPartialPixelZ(x & 0xF, y & 0xF, tileh);
	} else {
		return GetTileMaxPixelZ(tile);
	}
}

static Foundation GetFoundation_Object(TileIndex tile, Slope tileh)
{
	return IsObjectType(tile, OBJECT_OWNED_LAND) ? FOUNDATION_NONE : FlatteningFoundation(tileh);
}

/**
 * Perform the actual removal of the object from the map.
 * @param o The object to really clear.
 */
static void ReallyClearObjectTile(Object *o)
{
	Object::DecTypeCount(o->type);
	for (TileIndex tile_cur : o->location) {
		DeleteNewGRFInspectWindow(GSF_OBJECTS, tile_cur.base());

		MakeWaterKeepingClass(tile_cur, GetTileOwner(tile_cur));
	}
	delete o;
}

std::vector<ClearedObjectArea> _cleared_object_areas;

/**
 * Find the entry in _cleared_object_areas which occupies a certain tile.
 * @param tile Tile of interest
 * @return Occupying entry, or nullptr if none
 */
ClearedObjectArea *FindClearedObject(TileIndex tile)
{
	TileArea ta = TileArea(tile, 1, 1);

	for (ClearedObjectArea &coa : _cleared_object_areas) {
		if (coa.area.Intersects(ta)) return &coa;
	}

	return nullptr;
}

static CommandCost ClearTile_Object(TileIndex tile, DoCommandFlags flags)
{
	/* Get to the northern most tile. */
	Object *o = Object::GetByTile(tile);
	TileArea ta = o->location;

	ObjectType type = o->type;
	const ObjectSpec *spec = ObjectSpec::Get(type);

	CommandCost cost(EXPENSES_CONSTRUCTION, spec->GetClearCost() * ta.w * ta.h / 5);
	if (spec->flags.Test(ObjectFlag::ClearIncome)) cost.MultiplyCost(-1); // They get an income!

	/* Towns can't remove any objects. */
	if (_current_company == OWNER_TOWN) return CMD_ERROR;

	/* Water can remove everything! */
	if (_current_company != OWNER_WATER) {
		if (flags.Test(DoCommandFlag::NoWater) && IsTileOnWater(tile)) {
			/* There is water under the object, treat it as water tile. */
			return CommandCost(STR_ERROR_CAN_T_BUILD_ON_WATER);
		} else if (!spec->flags.Test(ObjectFlag::Autoremove) && flags.Test(DoCommandFlag::Auto)) {
			/* No automatic removal by overbuilding stuff. */
			return CommandCost(type == OBJECT_HQ ? STR_ERROR_COMPANY_HEADQUARTERS_IN : STR_ERROR_OBJECT_IN_THE_WAY);
		} else if (_game_mode == GM_EDITOR) {
			/* No further limitations for the editor. */
		} else if (GetTileOwner(tile) == OWNER_NONE) {
			/* Owned by nobody and unremovable, so we can only remove it with brute force! */
			if (!_cheats.magic_bulldozer.value && spec->flags.Test(ObjectFlag::CannotRemove)) return CMD_ERROR;
		} else if (CheckTileOwnership(tile).Failed()) {
			/* We don't own it!. */
			return CommandCost(STR_ERROR_OWNED_BY);
		} else if (spec->flags.Test(ObjectFlag::CannotRemove) && !spec->flags.Test(ObjectFlag::Autoremove)) {
			/* In the game editor or with cheats we can remove, otherwise we can't. */
			if (!_cheats.magic_bulldozer.value) {
				if (type == OBJECT_HQ) return CommandCost(STR_ERROR_COMPANY_HEADQUARTERS_IN);
				return CMD_ERROR;
			}

			/* Removing with the cheat costs more in TTDPatch / the specs. */
			cost.MultiplyCost(25);
		}
	} else if (spec->flags.Any({ObjectFlag::BuiltOnWater, ObjectFlag::NotOnLand})) {
		/* Water can't remove objects that are buildable on water. */
		return CMD_ERROR;
	}

	switch (type) {
		case OBJECT_HQ: {
			Company *c = Company::Get(GetTileOwner(tile));
			if (flags.Test(DoCommandFlag::Execute)) {
				c->location_of_HQ = INVALID_TILE; // reset HQ position
				SetWindowDirty(WC_COMPANY, c->index);
				CargoPacket::InvalidateAllFrom({c->index, SourceType::Headquarters});
			}

			/* cost of relocating company is 1% of company value */
			cost = CommandCost(EXPENSES_CONSTRUCTION, CalculateCompanyValue(c) / 100);
			break;
		}

		case OBJECT_STATUE:
			if (flags.Test(DoCommandFlag::Execute)) {
				Town *town = o->town;
				town->statues.Reset(GetTileOwner(tile));
				SetWindowDirty(WC_TOWN_AUTHORITY, town->index);
			}
			break;

		default:
			break;
	}

	_cleared_object_areas.emplace_back(tile, ta);

	if (flags.Test(DoCommandFlag::Execute)) ReallyClearObjectTile(o);

	return cost;
}

static void AddAcceptedCargo_Object(TileIndex tile, CargoArray &acceptance, CargoTypes &always_accepted)
{
	if (!IsObjectType(tile, OBJECT_HQ)) return;

	/* HQ accepts passenger and mail; but we have to divide the values
	 * between 4 tiles it occupies! */

	/* HQ level (depends on company performance) in the range 1..5. */
	uint level = GetCompanyHQSize(tile) + 1;

	/* Top town building generates 10, so to make HQ interesting, the top
	 * type makes 20. */
	CargoType pass = GetCargoTypeByLabel(CT_PASSENGERS);
	if (IsValidCargoType(pass)) {
		acceptance[pass] += std::max(1U, level);
		SetBit(always_accepted, pass);
	}

	/* Top town building generates 4, HQ can make up to 8. The
	 * proportion passengers:mail is different because such a huge
	 * commercial building generates unusually high amount of mail
	 * correspondence per physical visitor. */
	CargoType mail = GetCargoTypeByLabel(CT_MAIL);
	if (IsValidCargoType(mail)) {
		acceptance[mail] += std::max(1U, level / 2);
		SetBit(always_accepted, mail);
	}
}

static void AddProducedCargo_Object(TileIndex tile, CargoArray &produced)
{
	if (!IsObjectType(tile, OBJECT_HQ)) return;

	CargoType pass = GetCargoTypeByLabel(CT_PASSENGERS);
	if (IsValidCargoType(pass)) produced[pass]++;
	CargoType mail = GetCargoTypeByLabel(CT_MAIL);
	if (IsValidCargoType(mail)) produced[mail]++;
}


static void GetTileDesc_Object(TileIndex tile, TileDesc &td)
{
	const ObjectSpec *spec = ObjectSpec::GetByTile(tile);
	td.str = spec->name;
	td.owner[0] = GetTileOwner(tile);
	td.build_date = Object::GetByTile(tile)->build_date;

	if (spec->grf_prop.HasGrfFile()) {
		td.grf = GetGRFConfig(spec->grf_prop.grfid)->GetName();
	}
}

static void TileLoop_Object(TileIndex tile)
{
	const ObjectSpec *spec = ObjectSpec::GetByTile(tile);
	if (spec->flags.Test(ObjectFlag::Animation)) {
		Object *o = Object::GetByTile(tile);
		TriggerObjectTileAnimation(o, tile, ObjectAnimationTrigger::TileLoop, spec);
		if (o->location.tile == tile) TriggerObjectAnimation(o, ObjectAnimationTrigger::TileLoopNorth, spec);
	}

	if (IsTileOnWater(tile)) TileLoop_Water(tile);

	if (!IsObjectType(tile, OBJECT_HQ)) return;

	/* HQ accepts passenger and mail; but we have to divide the values
	 * between 4 tiles it occupies! */

	/* HQ level (depends on company performance) in the range 1..5. */
	uint level = GetCompanyHQSize(tile) + 1;
	assert(level < 6);

	StationFinder stations(TileArea(tile, 2, 2));

	uint r = Random();
	/* Top town buildings generate 250, so the top HQ type makes 256. */
	CargoType pass = GetCargoTypeByLabel(CT_PASSENGERS);
	if (IsValidCargoType(pass) && GB(r, 0, 8) < (256 / 4 / (6 - level))) {
		uint amt = GB(r, 0, 8) / 8 / 4 + 1;

		/* Production is halved during recessions. */
		if (EconomyIsInRecession()) amt = (amt + 1) >> 1;

		/* Scale by cargo scale setting. */
		amt = ScaleByCargoScale(amt, true);

		MoveGoodsToStation(pass, amt, {GetTileOwner(tile), SourceType::Headquarters}, stations.GetStations());
	}

	/* Top town building generates 90, HQ can make up to 196. The
	 * proportion passengers:mail is about the same as in the acceptance
	 * equations. */
	CargoType mail = GetCargoTypeByLabel(CT_MAIL);
	if (IsValidCargoType(mail) && GB(r, 8, 8) < (196 / 4 / (6 - level))) {
		uint amt = GB(r, 8, 8) / 8 / 4 + 1;

		/* Production is halved during recessions. */
		if (EconomyIsInRecession()) amt = (amt + 1) >> 1;

		/* Scale by cargo scale setting. */
		amt = ScaleByCargoScale(amt, true);

		MoveGoodsToStation(mail, amt, {GetTileOwner(tile), SourceType::Headquarters}, stations.GetStations());
	}
}


static TrackStatus GetTileTrackStatus_Object(TileIndex, TransportType, uint, DiagDirection)
{
	return 0;
}

static bool ClickTile_Object(TileIndex tile)
{
	if (!IsObjectType(tile, OBJECT_HQ)) return false;

	ShowCompany(GetTileOwner(tile));
	return true;
}

static void AnimateTile_Object(TileIndex tile)
{
	AnimateNewObjectTile(tile);
}

/**
 * Try to build a lighthouse.
 * @return True iff building a lighthouse succeeded.
 */
static bool TryBuildLightHouse()
{
	uint maxx = Map::MaxX();
	uint maxy = Map::MaxY();
	uint r = Random();

	/* Scatter the lighthouses more evenly around the perimeter */
	int perimeter = (GB(r, 16, 16) % (2 * (maxx + maxy))) - maxy;
	DiagDirection dir;
	for (dir = DIAGDIR_NE; perimeter > 0; dir++) {
		perimeter -= (DiagDirToAxis(dir) == AXIS_X) ? maxx : maxy;
	}

	TileIndex tile;
	switch (dir) {
		default:
		case DIAGDIR_NE: tile = TileXY(maxx - 1, r % maxy); break;
		case DIAGDIR_SE: tile = TileXY(r % maxx, 1); break;
		case DIAGDIR_SW: tile = TileXY(1,        r % maxy); break;
		case DIAGDIR_NW: tile = TileXY(r % maxx, maxy - 1); break;
	}

	/* Only build lighthouses at tiles where the border is sea. */
	if (!IsTileType(tile, MP_WATER)) return false;

	for (int j = 0; j < 19; j++) {
		int h;
		if (IsTileType(tile, MP_CLEAR) && IsTileFlat(tile, &h) && h <= 2 && !IsBridgeAbove(tile)) {
			BuildObject(OBJECT_LIGHTHOUSE, tile);
			assert(tile < Map::Size());
			return true;
		}
		tile += TileOffsByDiagDir(dir);
		if (!IsValidTile(tile)) return false;
	}
	return false;
}

/**
 * Try to build a transmitter.
 * @return True iff a transmitter was built.
 */
static bool TryBuildTransmitter()
{
	TileIndex tile = RandomTile();
	int h;
	if (IsTileType(tile, MP_CLEAR) && IsTileFlat(tile, &h) && h >= 4 && !IsBridgeAbove(tile)) {
		for (auto t : SpiralTileSequence(tile, 9)) {
			if (IsObjectTypeTile(t, OBJECT_TRANSMITTER)) return false;
		}
		BuildObject(OBJECT_TRANSMITTER, tile);
		return true;
	}
	return false;
}

void GenerateObjects()
{
	/* Set a guestimate on how much we progress */
	SetGeneratingWorldProgress(GWP_OBJECT, (uint)ObjectSpec::Count());

	/* Determine number of water tiles at map border needed for freeform_edges */
	uint num_water_tiles = 0;
	if (_settings_game.construction.freeform_edges) {
		for (uint x = 0; x < Map::MaxX(); x++) {
			if (IsTileType(TileXY(x, 1), MP_WATER)) num_water_tiles++;
			if (IsTileType(TileXY(x, Map::MaxY() - 1), MP_WATER)) num_water_tiles++;
		}
		for (uint y = 1; y < Map::MaxY() - 1; y++) {
			if (IsTileType(TileXY(1, y), MP_WATER)) num_water_tiles++;
			if (IsTileType(TileXY(Map::MaxX() - 1, y), MP_WATER)) num_water_tiles++;
		}
	}

	/* Iterate over all possible object types */
	for (const auto &spec : ObjectSpec::Specs()) {

		/* Continue, if the object was never available till now or shall not be placed */
		if (!spec.WasEverAvailable() || spec.generate_amount == 0) continue;

		uint16_t amount = spec.generate_amount;

		/* Scale by map size */
		if (spec.flags.Test(ObjectFlag::ScaleByWater) && _settings_game.construction.freeform_edges) {
			/* Scale the amount of lighthouses with the amount of land at the borders.
			 * The -6 is because the top borders are MP_VOID (-2) and all corners
			 * are counted twice (-4). */
			amount = Map::ScaleBySize1D(amount * num_water_tiles) / (2 * Map::MaxY() + 2 * Map::MaxX() - 6);
		} else if (spec.flags.Test(ObjectFlag::ScaleByWater)) {
			amount = Map::ScaleBySize1D(amount);
		} else {
			amount = Map::ScaleBySize(amount);
		}

		/* Now try to place the requested amount of this object */
		for (uint j = Map::ScaleBySize(1000); j != 0 && amount != 0 && Object::CanAllocateItem(); j--) {
			switch (spec.Index()) {
				case OBJECT_TRANSMITTER:
					if (TryBuildTransmitter()) amount--;
					break;

				case OBJECT_LIGHTHOUSE:
					if (TryBuildLightHouse()) amount--;
					break;

				default:
					uint8_t view = RandomRange(spec.views);
					if (CmdBuildObject({DoCommandFlag::Execute, DoCommandFlag::Auto, DoCommandFlag::NoTestTownRating, DoCommandFlag::NoModifyTownRating}, RandomTile(), spec.Index(), view).Succeeded()) amount--;
					break;
			}
		}
		IncreaseGeneratingWorldProgress(GWP_OBJECT);
	}
}

static void ChangeTileOwner_Object(TileIndex tile, Owner old_owner, Owner new_owner)
{
	if (!IsTileOwner(tile, old_owner)) return;

	bool do_clear = false;

	ObjectType type = GetObjectType(tile);
	if ((type == OBJECT_OWNED_LAND || type >= NEW_OBJECT_OFFSET) && new_owner != INVALID_OWNER) {
		SetTileOwner(tile, new_owner);
		if (GetWaterClass(tile) == WATER_CLASS_CANAL) {
			Company::Get(old_owner)->infrastructure.water--;
			Company::Get(new_owner)->infrastructure.water++;
		}
	} else if (type == OBJECT_STATUE) {
		Town *t = Object::GetByTile(tile)->town;
		t->statues.Reset(old_owner);
		if (new_owner != INVALID_OWNER && !t->statues.Test(new_owner)) {
			/* Transfer ownership to the new company */
			t->statues.Set(new_owner);
			SetTileOwner(tile, new_owner);
		} else {
			do_clear = true;
		}

		SetWindowDirty(WC_TOWN_AUTHORITY, t->index);
	} else {
		do_clear = true;
	}

	if (do_clear) {
		ReallyClearObjectTile(Object::GetByTile(tile));
		/* When clearing objects, they may turn into canal, which may require transferring ownership. */
		ChangeTileOwner(tile, old_owner, new_owner);
	}
}

static CommandCost TerraformTile_Object(TileIndex tile, DoCommandFlags flags, int z_new, Slope tileh_new)
{
	ObjectType type = GetObjectType(tile);

	if (type == OBJECT_OWNED_LAND) {
		/* Owned land remains unsold */
		CommandCost ret = CheckTileOwnership(tile);
		if (ret.Succeeded()) return CommandCost();
	} else if (AutoslopeEnabled() && type != OBJECT_TRANSMITTER && type != OBJECT_LIGHTHOUSE) {
		/* Behaviour:
		 *  - Both new and old slope must not be steep.
		 *  - TileMaxZ must not be changed.
		 *  - Allow autoslope by default.
		 *  - Disallow autoslope if callback succeeds and returns non-zero.
		 */
		Slope tileh_old = GetTileSlope(tile);
		/* TileMaxZ must not be changed. Slopes must not be steep. */
		if (!IsSteepSlope(tileh_old) && !IsSteepSlope(tileh_new) && (GetTileMaxZ(tile) == z_new + GetSlopeMaxZ(tileh_new))) {
			const ObjectSpec *spec = ObjectSpec::Get(type);

			/* Call callback 'disable autosloping for objects'. */
			if (spec->callback_mask.Test(ObjectCallbackMask::Autoslope)) {
				/* If the callback fails, allow autoslope. */
				uint16_t res = GetObjectCallback(CBID_OBJECT_AUTOSLOPE, 0, 0, spec, Object::GetByTile(tile), tile);
				if (res == CALLBACK_FAILED || !ConvertBooleanCallback(spec->grf_prop.grffile, CBID_OBJECT_AUTOSLOPE, res)) return CommandCost(EXPENSES_CONSTRUCTION, _price[PR_BUILD_FOUNDATION]);
			} else if (spec->IsEnabled()) {
				/* allow autoslope */
				return CommandCost(EXPENSES_CONSTRUCTION, _price[PR_BUILD_FOUNDATION]);
			}
		}
	}

	return Command<CMD_LANDSCAPE_CLEAR>::Do(flags, tile);
}

extern const TileTypeProcs _tile_type_object_procs = {
	DrawTile_Object,             // draw_tile_proc
	GetSlopePixelZ_Object,       // get_slope_z_proc
	ClearTile_Object,            // clear_tile_proc
	AddAcceptedCargo_Object,     // add_accepted_cargo_proc
	GetTileDesc_Object,          // get_tile_desc_proc
	GetTileTrackStatus_Object,   // get_tile_track_status_proc
	ClickTile_Object,            // click_tile_proc
	AnimateTile_Object,          // animate_tile_proc
	TileLoop_Object,             // tile_loop_proc
	ChangeTileOwner_Object,      // change_tile_owner_proc
	AddProducedCargo_Object,     // add_produced_cargo_proc
	nullptr,                        // vehicle_enter_tile_proc
	GetFoundation_Object,        // get_foundation_proc
	TerraformTile_Object,        // terraform_tile_proc
};
