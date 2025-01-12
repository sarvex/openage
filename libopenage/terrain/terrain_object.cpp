// Copyright 2013-2023 the openage authors. See copying.md for legal info.

#include "terrain_object.h"

#include <algorithm>
#include <cmath>

#include "../coord/phys.h"
#include "../coord/pixel.h"
#include "../coord/tile.h"
#include "../engine.h"
#include "../error/error.h"
#include "../texture.h"
#include "../unit/unit.h"

#include "terrain.h"
#include "terrain_chunk.h"
#include "terrain_outline.h"

namespace openage {

TerrainObject::TerrainObject(Unit &u) :
	unit(u),
	passable{[](const coord::phys3 &) -> bool { return true; }},
	draw{[](const LegacyEngine & /*e*/) {}},
	state{object_state::removed},
	occupied_chunk_count{0},
	parent{nullptr} {
}

TerrainObject::~TerrainObject() {
	// remove all connections from terrain
	this->unit.log(MSG(dbg) << "Cleanup terrain object");
	this->remove();
}

bool TerrainObject::is_floating() const {
	// if parent is floating then all children also are
	if (this->parent && this->parent->is_floating()) {
		return true;
	}
	return this->state == object_state::floating;
}

bool TerrainObject::is_placed() const {
	// if object has a parent it must be placed
	if (this->parent && !this->parent->is_placed()) {
		return false;
	}
	return this->state == object_state::placed || this->state == object_state::placed_no_collision;
}


bool TerrainObject::check_collisions() const {
	// if object has a parent it must be placed
	if (this->parent && !this->parent->is_placed()) {
		return false;
	}
	return this->state == object_state::placed;
}

void TerrainObject::draw_outline(const coord::CoordManager &coord) const {
	this->outline_texture->draw(coord, this->pos.draw);
}

bool TerrainObject::place(object_state init_state) {
	if (this->state == object_state::removed) {
		throw Error(MSG(err) << "Building cannot change state with no position");
	}

	// remove any other floating objects
	// which intersect with the new placement
	// if non-floating objects are on the foundation
	// then this placement will fail
	for (coord::tile temp_pos : tile_list(this->pos)) {
		std::vector<TerrainObject *> to_remove;
		TerrainChunk *chunk = this->get_terrain()->get_chunk(temp_pos);

		if (chunk == nullptr) {
			continue;
		}

		for (auto obj : chunk->get_data(temp_pos)->obj) {
			// ignore self and annexes of self
			if (obj != this && obj->get_parent() != this) {
				if (obj->is_floating()) {
					// floating objects get removed
					to_remove.push_back(obj);
				}
				else if (obj->check_collisions()) {
					// solid objects obstruct placement
					return false;
				}
			}
		}

		// all obstructing objects get deleted
		for (auto remove_obj : to_remove) {
			remove_obj->unit.location = nullptr;
		}
	}

	// set new state
	this->state = init_state;
	return true;
}

bool TerrainObject::place(const std::shared_ptr<Terrain> &t, coord::phys3 &position, object_state init_state) {
	if (this->state != object_state::removed) {
		throw Error(MSG(err) << "This object has already been placed.");
	}
	else if (init_state == object_state::removed) {
		throw Error(MSG(err) << "Cannot place an object with removed state.");
	}

	// use passiblity test
	if (not this->passable(position)) {
		return false;
	}

	// place on terrain
	this->place_unchecked(t, position);

	// set state
	this->state = init_state;
	return true;
}

bool TerrainObject::move(coord::phys3 &position) {
	if (this->state == object_state::removed) {
		return false;
	}
	auto old_state = this->state;

	// TODO should do outside of this function
	bool can_move = this->passable(position);
	if (can_move) {
		this->remove();
		this->place_unchecked(this->get_terrain(), position);
		this->state = old_state;
	}
	return can_move;
}

void TerrainObject::remove() {
	// remove all children first
	for (auto &c : this->children) {
		c->remove();
	}
	this->children.clear();

	if (this->occupied_chunk_count == 0 || this->state == object_state::removed) {
		return;
	}

	for (coord::tile temp_pos : tile_list(this->pos)) {
		TerrainChunk *chunk = this->get_terrain()->get_chunk(temp_pos);

		if (chunk == nullptr) {
			continue;
		}

		auto &v = chunk->get_data(temp_pos.get_pos_on_chunk())->obj;
		auto position_it = std::remove_if(
			std::begin(v),
			std::end(v),
			[this](TerrainObject *obj) {
				return this == obj;
			});
		v.erase(position_it, std::end(v));
	}

	this->occupied_chunk_count = 0;
	this->state = object_state::removed;
}

void TerrainObject::set_ground(int id, int additional) {
	if (not this->is_placed()) {
		throw Error(MSG(err) << "Setting ground for object that is not placed yet.");
	}

	coord::tile temp_pos = this->pos.start;
	temp_pos.ne -= additional;
	temp_pos.se -= additional;
	while (temp_pos.ne < this->pos.end.ne + additional) {
		while (temp_pos.se < this->pos.end.se + additional) {
			TerrainChunk *chunk = this->get_terrain()->get_chunk(temp_pos);

			if (chunk == nullptr) {
				continue;
			}

			chunk->get_data(temp_pos.get_pos_on_chunk())->terrain_id = id;
			temp_pos.se++;
		}
		temp_pos.se = this->pos.start.se - additional;
		temp_pos.ne++;
	}
}

const TerrainObject *TerrainObject::get_parent() const {
	return this->parent;
}

std::vector<TerrainObject *> TerrainObject::get_children() const {
	// TODO: a better performing way of doing this
	// for example accept a lambda to use for each element
	// or maintain a duplicate class field for raw pointers

	std::vector<TerrainObject *> result;
	for (auto &obj : this->children) {
		result.push_back(obj.get());
	}
	return result;
}

bool TerrainObject::operator<(const TerrainObject &other) {
	if (this == &other) {
		return false;
	}

	auto this_ne = this->pos.draw.ne;
	auto this_se = this->pos.draw.se;
	auto other_ne = other.pos.draw.ne;
	auto other_se = other.pos.draw.se;

	auto this_ypos = this_ne - this_se;
	auto other_ypos = other_ne - other_se;

	if (this_ypos < other_ypos) {
		return false;
	}
	else if (this_ypos == other_ypos) {
		if (this_ne > other_ne) {
			return false;
		}
		else if (this_ne == other_ne) {
			return this_se > other_se;
		}
	}

	return true;
}

void TerrainObject::place_unchecked(const std::shared_ptr<Terrain> &t, coord::phys3 &position) {
	// storing the position:
	this->pos = this->get_range(position, *t);
	this->terrain = t;
	this->occupied_chunk_count = 0;

	bool chunk_known = false;


	// set pointers to this object on each terrain tile
	// where the building will stand and block the ground
	for (coord::tile temp_pos : tile_list(this->pos)) {
		TerrainChunk *chunk = this->get_terrain()->get_chunk(temp_pos);

		if (chunk == nullptr) {
			continue;
		}

		for (int c = 0; c < this->occupied_chunk_count; c++) {
			if (this->occupied_chunk[c] == chunk) {
				chunk_known = true;
			}
		}

		if (not chunk_known) {
			this->occupied_chunk[this->occupied_chunk_count] = chunk;
			this->occupied_chunk_count += 1;
		}
		else {
			chunk_known = false;
		}

		chunk->get_data(temp_pos.get_pos_on_chunk())->obj.push_back(this);
	}
}

SquareObject::SquareObject(Unit &u, coord::tile_delta foundation_size) :
	SquareObject(u, foundation_size, square_outline(foundation_size)) {
}

SquareObject::SquareObject(Unit &u, coord::tile_delta foundation_size, std::shared_ptr<Texture> out_tex) :
	TerrainObject(u),
	size(foundation_size) {
	this->outline_texture = out_tex;
}

SquareObject::~SquareObject() = default;

tile_range SquareObject::get_range(const coord::phys3 &pos, const Terrain &terrain) const {
	return building_center(pos, this->size, terrain);
}

coord::phys_t SquareObject::from_edge(const coord::phys3 &point) const {
	// clamp between start and end
	coord::phys2 start_phys = this->pos.start.to_phys2();
	coord::phys2 end_phys = this->pos.end.to_phys2();

	coord::phys_t cx = std::max(start_phys.ne, std::min(end_phys.ne, point.ne));
	coord::phys_t cy = std::max(start_phys.se, std::min(end_phys.se, point.se));

	// distance to clamped point
	coord::phys_t dx = point.ne - cx;
	coord::phys_t dy = point.se - cy;
	return std::hypot(dx, dy);
}

coord::phys3 SquareObject::on_edge(const coord::phys3 &angle, coord::phys_t /* extra */) const {
	// clamp between start and end
	// TODO extra is unused
	coord::phys2 start_phys = this->pos.start.to_phys2();
	coord::phys2 end_phys = this->pos.end.to_phys2();
	coord::phys_t cx = std::max(start_phys.ne, std::min(end_phys.ne, angle.ne));
	coord::phys_t cy = std::max(start_phys.se, std::min(end_phys.se, angle.se));

	// todo use extra distance
	return coord::phys3{cx, cy, 0};
}

bool SquareObject::contains(const coord::phys3 &other) const {
	coord::tile other_tile = other.to_tile3().to_tile();

	for (coord::tile check_pos : tile_list(this->pos)) {
		if (check_pos == other_tile) {
			return true;
		}
	}
	return false;
}

bool SquareObject::intersects(const TerrainObject &other, const coord::phys3 &position) const {
	if (const auto *sq = dynamic_cast<const SquareObject *>(&other)) {
		auto terrain_ptr = this->terrain.lock();
		if (not terrain_ptr) {
			throw Error{ERR << "object is not associated to a valid terrain"};
		}

		tile_range rng = this->get_range(position, *terrain_ptr);
		return this->pos.end.ne < rng.start.ne
		       || rng.end.ne < sq->pos.start.ne
		       || rng.end.se < sq->pos.start.se
		       || rng.end.se < sq->pos.start.se;
	}
	else if (const auto *rad = dynamic_cast<const RadialObject *>(&other)) {
		auto terrain_ptr = this->terrain.lock();
		if (not terrain_ptr) {
			throw Error{ERR << "object is not associated to a valid terrain"};
		}
		// clamp between start and end
		tile_range rng = this->get_range(position, *terrain_ptr);
		coord::phys2 start_phys = rng.start.to_phys2();
		coord::phys2 end_phys = rng.end.to_phys2();
		coord::phys_t cx = std::max(start_phys.ne, std::min(end_phys.ne, rad->pos.draw.ne));
		coord::phys_t cy = std::max(start_phys.se, std::min(end_phys.se, rad->pos.draw.se));

		// distance to square object base
		coord::phys_t dx = rad->pos.draw.ne - cx;
		coord::phys_t dy = rad->pos.draw.se - cy;
		return std::hypot(dx, dy) < rad->phys_radius.to_double();
	}
	return false;
}

coord::phys_t SquareObject::min_axis() const {
	return std::min(this->size.ne, this->size.se);
}

RadialObject::RadialObject(Unit &u, float rad) :
	RadialObject(u, rad, radial_outline(rad)) {
}

RadialObject::RadialObject(Unit &u, float rad, std::shared_ptr<Texture> out_tex) :
	TerrainObject(u),
	phys_radius(rad) {
	this->outline_texture = out_tex;
}

RadialObject::~RadialObject() = default;

tile_range RadialObject::get_range(const coord::phys3 &pos, const Terrain & /*terrain*/) const {
	tile_range result;

	// create bounds
	coord::phys3 p_start = pos, p_end = pos;
	p_start.ne -= this->phys_radius;
	p_start.se -= this->phys_radius;
	p_end.ne += this->phys_radius;
	p_end.se += this->phys_radius;

	// set result
	result.start = p_start.to_tile3().to_tile();
	result.end = p_end.to_tile3().to_tile() + coord::tile_delta{1, 1};
	result.draw = pos;
	return result;
}

coord::phys_t RadialObject::from_edge(const coord::phys3 &point) const {
	return std::max(
		coord::phys_t(point.to_phys2().distance(this->pos.draw.to_phys2())) - this->phys_radius,
		static_cast<coord::phys_t>(0));
}

coord::phys3 RadialObject::on_edge(const coord::phys3 &angle, coord::phys_t extra) const {
	return this->pos.draw + (angle - this->pos.draw).to_phys2().normalize((this->phys_radius + extra).to_double()).to_phys3();
}

bool RadialObject::contains(const coord::phys3 &other) const {
	return this->pos.draw.to_phys2().distance(other.to_phys2()) < this->phys_radius.to_double();
}

bool RadialObject::intersects(const TerrainObject &other, const coord::phys3 &position) const {
	if (const auto *sq = dynamic_cast<const SquareObject *>(&other)) {
		return sq->from_edge(position) < this->phys_radius;
	}
	else if (const auto *rad = dynamic_cast<const RadialObject *>(&other)) {
		return position.to_phys2().distance(rad->pos.draw.to_phys2()) < (this->phys_radius + rad->phys_radius).to_double();
	}
	return false;
}

coord::phys_t RadialObject::min_axis() const {
	return this->phys_radius * 2;
}

std::vector<coord::tile> tile_list(const tile_range &rng) {
	std::vector<coord::tile> tiles;

	coord::tile check_pos = rng.start;
	while (check_pos.ne < rng.end.ne) {
		while (check_pos.se < rng.end.se) {
			tiles.push_back(check_pos);
			check_pos.se += 1;
		}
		check_pos.se = rng.start.se;
		check_pos.ne += 1;
	}

	// a case when the objects radius is zero
	if (tiles.empty()) {
		tiles.push_back(rng.start);
	}
	return tiles;
}

tile_range building_center(coord::phys3 west, coord::tile_delta size, const Terrain &terrain) {
	tile_range result;

	// TODO it should be possible that the building is placed on any position,
	//      not just tile positions.
	result.start = west.to_tile();
	result.end = result.start + size;

	coord::phys2 draw_pos = result.start.to_phys2();

	draw_pos.ne += coord::phys_t(size.ne / 2.0f);
	draw_pos.se += coord::phys_t(size.se / 2.0f);

	result.draw = draw_pos.to_phys3(terrain);
	return result;
}

bool complete_building(Unit &u) {
	if (u.has_attribute(attr_type::building)) {
		auto &build = u.get_attribute<attr_type::building>();
		build.completed = 1.0f;

		// set ground under a completed building
		auto target_location = u.location.get();
		bool placed_ok = target_location->place(build.completion_state);
		if (placed_ok) {
			target_location->set_ground(build.foundation_terrain, 0);
		}
		return placed_ok;
	}
	return false;
}

} // namespace openage
