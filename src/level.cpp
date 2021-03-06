/*
	Copyright (C) 2003-2013 by David White <davewx7@gmail.com>
	
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <boost/bind.hpp>

#include <algorithm>
#include <iostream>
#include <math.h>

#include "IMG_savepng.h"
#include "asserts.hpp"
#include "collision_utils.hpp"
#include "controls.hpp"
#include "draw_scene.hpp"
#include "draw_tile.hpp"
#include "editor.hpp"
#include "entity.hpp"
#include "filesystem.hpp"
#include "foreach.hpp"
#include "formatter.hpp"
#include "formula_profiler.hpp"
#include "gui_formula_functions.hpp"
#include "hex_map.hpp"
#include "hex_object.hpp"
#include "iphone_controls.hpp"
#include "json_parser.hpp"
#include "level.hpp"
#include "level_object.hpp"
#include "level_runner.hpp"
#include "light.hpp"
#include "load_level.hpp"
#include "module.hpp"
#include "multiplayer.hpp"
#include "object_events.hpp"
#include "player_info.hpp"
#include "playable_custom_object.hpp"
#include "preferences.hpp"
#include "preprocessor.hpp"
#include "random.hpp"
#include "raster.hpp"
#include "sound.hpp"
#include "stats.hpp"
#include "string_utils.hpp"
#include "surface_palette.hpp"
#include "texture_frame_buffer.hpp"
#include "thread.hpp"
#include "tile_map.hpp"
#include "unit_test.hpp"
#include "variant_utils.hpp"
#include "wml_formula_callable.hpp"
#include "color_utils.hpp"

#include "compat.hpp"

#ifndef NO_EDITOR
std::set<level*>& get_all_levels_set() {
	static std::set<level*> all;
	return all;
}
#endif

namespace {

PREF_BOOL(debug_shadows, false, "Show debug visualization of shadow drawing");


boost::intrusive_ptr<level>& get_current_level() {
	static boost::intrusive_ptr<level> current_level;
	return current_level;
}

std::map<std::string, level::summary> load_level_summaries() {
	std::map<std::string, level::summary> result;
	const variant node = json::parse_from_file("data/compiled/level_index.cfg");
	
	foreach(variant level_node, node["level"].as_list()) {
		level::summary& s = result[level_node["level"].as_string()];
		s.music = level_node["music"].as_string();
		s.title = level_node["title"].as_string();
	}

	return result;
}

bool level_tile_not_in_rect(const rect& r, const level_tile& t) {
	return t.x < r.x() || t.y < r.y() || t.x >= r.x2() || t.y >= r.y2();
}

}

void level::clear_current_level()
{
	get_current_level().reset();
}

level::summary level::get_summary(const std::string& id)
{
	static const std::map<std::string, summary> summaries = load_level_summaries();
	std::map<std::string, summary>::const_iterator i = summaries.find(id);
	if(i != summaries.end()) {
		return i->second;
	}

	return summary();
}

level& level::current()
{
	ASSERT_LOG(get_current_level(), "Tried to query current level when there is none");
	return *get_current_level();
}

level* level::current_ptr()
{
	return get_current_level().get();
}

current_level_scope::current_level_scope(level* lvl) : old_(get_current_level())
{
	lvl->set_as_current_level();
}

current_level_scope::~current_level_scope() {
	if(old_) {
		old_->set_as_current_level();
	}
}

void level::set_as_current_level()
{
	get_current_level() = this;
	frame::set_color_palette(palettes_used_);

	if(false && preferences::auto_size_window()) {
		static bool auto_sized = false;
		if(!auto_sized) {
			auto_sized = true;
		}

		int w,h;
		get_main_window()->auto_window_size(w,h);
		get_main_window()->set_window_size(w,h);
	}

#if !TARGET_OS_IPHONE && !TARGET_BLACKBERRY
#ifndef NO_EDITOR
	static const int starting_x_resolution = preferences::actual_screen_width();
	static const int starting_y_resolution = preferences::actual_screen_height();
	static const int starting_virtual_x_resolution = preferences::virtual_screen_width();
	static const int starting_virtual_y_resolution = preferences::virtual_screen_height();

	if(set_screen_resolution_on_entry_ && !editor_ && !editor_resolution_manager::is_active() && starting_x_resolution == starting_virtual_x_resolution && !preferences::auto_size_window()) {
		if(!x_resolution_) {
			x_resolution_ = starting_x_resolution;
		}

		if(!y_resolution_) {
			y_resolution_ = starting_y_resolution;
		}

		if(x_resolution_ != preferences::actual_screen_width() || y_resolution_ != preferences::actual_screen_height()) {

			std::cerr << "RESETTING VIDEO MODE: " << x_resolution_ << ", " << y_resolution_ << "\n";
			get_main_window()->set_window_size(x_resolution_, y_resolution_);
		}
	}
	
#endif // !NO_EDITOR
#endif

#if defined(USE_BOX2D)
	//for(std::vector<box2d::body_ptr>::iterator it = bodies_.begin(); it != bodies_.end(); ++it) {
	//	(*it)->recreate();
	//	std::cerr << "level body recreate: " << std::hex << intptr_t((*it).get()) << " " << intptr_t((*it)->get_body_ptr()) << std::dec << std::endl;
	//}
	
#endif
}

namespace {
graphics::color_transform default_dark_color() {
	return graphics::color_transform(0, 0, 0, 0);
}

variant_type_ptr g_player_type;
}

level::level(const std::string& level_cfg, variant node)
	: id_(level_cfg),
	  x_resolution_(0), y_resolution_(0),
	  set_screen_resolution_on_entry_(false),
	  highlight_layer_(INT_MIN),
	  num_compiled_tiles_(0),
	  entered_portal_active_(false), save_point_x_(-1), save_point_y_(-1),
	  editor_(false), show_foreground_(true), show_background_(true), dark_(false), dark_color_(graphics::color_transform(0, 0, 0, 255)), air_resistance_(0), water_resistance_(7), end_game_(false),
      editor_tile_updates_frozen_(0), editor_dragging_objects_(false),
	  zoom_level_(decimal::from_int(1)),
	  palettes_used_(0),
	  background_palette_(-1),
	  segment_width_(0), segment_height_(0),
#if defined(USE_ISOMAP)
	  mouselook_enabled_(false), mouselook_inverted_(false),
#endif
	  allow_touch_controls_(true)
{
#ifndef NO_EDITOR
	get_all_levels_set().insert(this);
#endif

	std::cerr << "in level constructor...\n";
	const int start_time = SDL_GetTicks();

	if(node.is_null()) {
		node = load_level_wml(level_cfg);
	}

	variant player_save_node;
	ASSERT_LOG(node.is_null() == false, "LOAD LEVEL WML FOR " << level_cfg << " FAILED");
	if(node.has_key("id")) {
		id_ = node["id"].as_string();
	}

#if defined(USE_SHADERS)
	if(node.has_key("shader")) {
		shader_.reset(new gles2::shader_program(node["shader"]));
	} else {
		shader_.reset();
	}
#endif

#if defined(USE_ISOMAP)
	if(node.has_key("camera")) {
		camera_.reset(new camera_callable(node["camera"]));
	} else {
		camera_.reset(new camera_callable());
	}

	if(node.has_key("isoworld")) {
		iso_world_.reset(new voxel::world(node["isoworld"]));
	} else {
		iso_world_.reset();
	}
#endif

	if(preferences::load_compiled() && (level_cfg == "save.cfg" || level_cfg == "autosave.cfg")) {
		if(preferences::version() != node["version"].as_string()) {
			std::cerr << "DIFFERENT VERSION LEVEL\n";
			foreach(variant obj_node, node["character"].as_list()) {
				if(obj_node["is_human"].as_bool(false)) {
					player_save_node = obj_node;
					break;
				}
			}

			variant n = node;
			if(node.has_key("id")) {
				n = load_level_wml(node["id"].as_string());
			}

			n = n.add_attr(variant("serialized_objects"), n["serialized_objects"] + node["serialized_objects"]);

			node = n;
		}
	}

	dark_color_ = default_dark_color();
	if(node["dark"].as_bool(false)) {
		dark_ = true;
	}

	if(node.has_key("dark_color")) {
		dark_color_ = graphics::color_transform(node["dark_color"]);
	}

	vars_ = node["vars"];
	if(vars_.is_map() == false) {
		std::map<variant,variant> m;
		vars_ = variant(&m);
	}

	segment_width_ = node["segment_width"].as_int();
	ASSERT_LOG(segment_width_%TileSize == 0, "segment_width in " << id_ << " is not divisible by " << TileSize << " (" << segment_width_%TileSize << " wide)");

	segment_height_ = node["segment_height"].as_int();
	ASSERT_LOG(segment_height_%TileSize == 0, "segment_height in " << id_ << " is not divisible by " << TileSize  << " (" << segment_height_%TileSize << " tall)");

	music_ = node["music"].as_string_default();
	replay_data_ = node["replay_data"].as_string_default();
	cycle_ = node["cycle"].as_int();
	paused_ = false;
	time_freeze_ = 0;
	x_resolution_ = node["x_resolution"].as_int();
	y_resolution_ = node["y_resolution"].as_int();
	set_screen_resolution_on_entry_ = node["set_screen_resolution_on_entry"].as_bool(false);
	in_dialog_ = false;
	title_ = node["title"].as_string_default();
	if(node.has_key("dimensions")) {
		boundaries_ = rect(node["dimensions"]);
	} else {
		boundaries_ = rect(0, 0, node["width"].as_int(799), node["height"].as_int(599));
	}

	if(node.has_key("lock_screen")) {
		lock_screen_.reset(new point(node["lock_screen"].as_string()));
	}

	if(node.has_key("opaque_rects")) {
		const std::vector<std::string> opaque_rects_str = util::split(node["opaque_rects"].as_string(), ':');
		foreach(const std::string& r, opaque_rects_str) {
			opaque_rects_.push_back(rect(r));
			std::cerr << "OPAQUE RECT: " << r << "\n";
		}
	}

	xscale_ = node["xscale"].as_int(100);
	yscale_ = node["yscale"].as_int(100);
	auto_move_camera_ = point(node["auto_move_camera"]);
	air_resistance_ = node["air_resistance"].as_int(20);
	water_resistance_ = node["water_resistance"].as_int(100);

	camera_rotation_ = game_logic::formula::create_optional_formula(node["camera_rotation"]);

	preloads_ = util::split(node["preloads"].as_string());

	std::string empty_solid_info;
	foreach(variant rect_node, node["solid_rect"].as_list()) {
		solid_rect r;
		r.r = rect(rect_node["rect"]);
		r.friction = rect_node["friction"].as_int(100);
		r.traction = rect_node["traction"].as_int(100);
		r.damage = rect_node["damage"].as_int();
		solid_rects_.push_back(r);
		add_solid_rect(r.r.x(), r.r.y(), r.r.x2(), r.r.y2(), r.friction, r.traction, r.damage, empty_solid_info);
	}

	std::cerr << "building..." << SDL_GetTicks() << "\n";
	widest_tile_ = 0;
	highest_tile_ = 0;
	layers_.insert(0);
	foreach(variant tile_node, node["tile"].as_list()) {
		const level_tile t = level_object::build_tile(tile_node);
		tiles_.push_back(t);
		layers_.insert(t.zorder);
		add_tile_solid(t);
	}
	std::cerr << "done building..." << SDL_GetTicks() << "\n";

	int begin_tile_index = tiles_.size();
	foreach(variant tile_node, node["tile_map"].as_list()) {
		variant tiles_value = tile_node["tiles"];
		if(!tiles_value.is_string()) {
			continue;
		}

		const std::string& str = tiles_value.as_string();
		bool contains_data = false;
		foreach(char c, str) {
			if(c != ',' && !util::c_isspace(c)) {
				contains_data = true;
				break;
			}
		}

		if(!contains_data) {
			continue;
		}

		tile_map m(tile_node);
		ASSERT_LOG(tile_maps_.count(m.zorder()) == 0, "repeated zorder in tile map: " << m.zorder());
		tile_maps_[m.zorder()] = m;
		const int before = tiles_.size();
		tile_maps_[m.zorder()].build_tiles(&tiles_);
		std::cerr << "LAYER " << m.zorder() << " BUILT " << (tiles_.size() - before) << " tiles\n";
	}

	std::cerr << "done building tile_map..." << SDL_GetTicks() << "\n";

	num_compiled_tiles_ = node["num_compiled_tiles"].as_int();

	tiles_.resize(tiles_.size() + num_compiled_tiles_);
	std::vector<level_tile>::iterator compiled_itor = tiles_.end() - num_compiled_tiles_;

	foreach(variant tile_node, node["compiled_tiles"].as_list()) {
		read_compiled_tiles(tile_node, compiled_itor);
		wml_compiled_tiles_.push_back(tile_node);
	}

	ASSERT_LOG(compiled_itor == tiles_.end(), "INCORRECT NUMBER OF COMPILED TILES");

	for(int i = begin_tile_index; i != tiles_.size(); ++i) {
		add_tile_solid(tiles_[i]);
		layers_.insert(tiles_[i].zorder);
	}

	if(std::adjacent_find(tiles_.rbegin(), tiles_.rend(), level_tile_zorder_pos_comparer()) != tiles_.rend()) {
		std::sort(tiles_.begin(), tiles_.end(), level_tile_zorder_pos_comparer());
	}

	///////////////////////
	// hex tiles starts
	foreach(variant tile_node, node["hex_tile_map"].as_list()) {
		hex::hex_map_ptr m(new hex::hex_map(tile_node));
		hex_maps_[m->zorder()] = m;
		//tile_maps_[m.zorder()].build_tiles();
		std::cerr << "LAYER " << m->zorder() << " BUILT " << hex_maps_[m->zorder()]->size() << " tiles\n";
		hex_maps_[m->zorder()]->build();
	}
	std::cerr << "done building hex_tile_map..." << SDL_GetTicks() << "\n";
	// hex tiles ends
	///////////////////////

	if(node.has_key("palettes")) {
		std::vector<std::string> v = parse_variant_list_or_csv_string(node["palettes"]);
		foreach(const std::string& p, v) {
			const int id = graphics::get_palette_id(p);
			palettes_used_ |= (1 << id);
		}
	}

	if(node.has_key("background_palette")) {
		background_palette_ = graphics::get_palette_id(node["background_palette"].as_string());
	}

	prepare_tiles_for_drawing();

	foreach(variant char_node, node["character"].as_list()) {
		if(player_save_node.is_null() == false && char_node["is_human"].as_bool(false)) {
			continue;
		}

		wml_chars_.push_back(char_node);
		continue;
	}

	if(player_save_node.is_null() == false) {
		wml_chars_.push_back(player_save_node);
	}

	variant serialized_objects = node["serialized_objects"];
	if(serialized_objects.is_null() == false) {
		serialized_objects_.push_back(serialized_objects);
	}

	foreach(variant portal_node, node["portal"].as_list()) {
		portal p;
		p.area = rect(portal_node["rect"]);
		p.level_dest = portal_node["level"].as_string();
		p.dest = point(portal_node["dest"].as_string());
		p.dest_starting_pos = portal_node["dest_starting_post"].as_bool(false);
		p.automatic = portal_node["automatic"].as_bool(true);
		p.transition = portal_node["transition"].as_string();
		portals_.push_back(p);
	}

	if(node.has_key("next_level")) {
		right_portal_.level_dest = node["next_level"].as_string();
		right_portal_.dest_str = "left";
		right_portal_.dest_starting_pos = false;
		right_portal_.automatic = true;
	}

	if(node.has_key("previous_level")) {
		left_portal_.level_dest = node["previous_level"].as_string();
		left_portal_.dest_str = "right";
		left_portal_.dest_starting_pos = false;
		left_portal_.automatic = true;
	}

	variant bg = node["background"];
	if(bg.is_map()) {
		background_.reset(new background(bg, background_palette_));
	} else if(node.has_key("background")) {
		background_ = background::get(node["background"].as_string(), background_palette_);
		background_offset_ = point(node["background_offset"]);
		background_->set_offset(background_offset_);
	}

	if(node.has_key("water")) {
		water_.reset(new water(node["water"]));
	}

	foreach(variant script_node, node["script"].as_list()) {
		movement_script s(script_node);
		movement_scripts_[s.id()] = s;
	}

	if(node.has_key("gui")) {
		if(node["gui"].is_string()) {
			gui_algo_str_.push_back(node["gui"].as_string());
		} else if(node["gui"].is_list()) {
			gui_algo_str_ = node["gui"].as_list_string();
		} else {
			ASSERT_LOG(false, "Unexpected type error for gui node " << level_cfg);
		}
	} else {
		gui_algo_str_.push_back("default");
	}

	foreach(const std::string& s, gui_algo_str_) {
		gui_algorithm_.push_back(gui_algorithm::get(s));
		gui_algorithm_.back()->new_level();
	}

	sub_level_str_ = node["sub_levels"].as_string_default();
	foreach(const std::string& sub_lvl, util::split(sub_level_str_)) {
		sub_level_data& data = sub_levels_[sub_lvl];
		data.lvl = boost::intrusive_ptr<level>(new level(sub_lvl + ".cfg"));
		foreach(int layer, data.lvl->layers_) {
			layers_.insert(layer);
		}

		data.active = false;
		data.xoffset = data.yoffset = 0;
		data.xbase = data.ybase = 0;
	}

	allow_touch_controls_ = node["touch_controls"].as_bool(true);

#ifdef USE_BOX2D
	if(node.has_key("bodies") && node["bodies"].is_list()) {
		for(int n = 0; n != node["bodies"].num_elements(); ++n) {
			bodies_.push_back(new box2d::body(node["bodies"][n]));
			std::cerr << "level create body: " << std::hex << intptr_t(bodies_.back().get()) << " " << intptr_t(bodies_.back()->get_raw_body_ptr()) << std::dec << std::endl;
		}
	}
#endif

	const int time_taken_ms = (SDL_GetTicks() - start_time);
	stats::entry("load", id()).set("time", variant(time_taken_ms));
	std::cerr << "done level constructor: " << time_taken_ms << "\n";
}

level::~level()
{
#ifndef NO_EDITOR
	get_all_levels_set().erase(this);
#endif

	for(std::deque<backup_snapshot_ptr>::iterator i = backups_.begin();
	    i != backups_.end(); ++i) {
		foreach(const entity_ptr& e, (*i)->chars) {
			//kill off any references this entity holds, to workaround
			//circular references causing things to stick around.
			e->cleanup_references();
		}
	}

	if(before_pause_controls_backup_) {
		before_pause_controls_backup_->cancel();
	}
}

void level::read_compiled_tiles(variant node, std::vector<level_tile>::iterator& out)
{
	const int xbase = node["x"].as_int();
	const int ybase = node["y"].as_int();
	const int zorder = parse_zorder(node["zorder"]);

	int x = xbase;
	int y = ybase;
	const std::string& tiles = node["tiles"].as_string();
	const char* i = tiles.c_str();
	const char* end = tiles.c_str() + tiles.size();
	while(i != end) {
		if(*i == '|') {
			++i;
		} else if(*i == ',') {
			x += TileSize;
			++i;
		} else if(*i == '\n') {
			x = xbase;
			y += TileSize;
			++i;
		} else {
			ASSERT_LOG(out != tiles_.end(), "NOT ENOUGH COMPILED TILES REPORTED");

			out->x = x;
			out->y = y;
			out->zorder = zorder;
			out->face_right = false;
			out->draw_disabled = false;
			if(*i == '~') {
				out->face_right = true;
				++i;
			}

			ASSERT_LOG(end - i >= 3, "ILLEGAL TILE FOUND");

			out->object = level_object::get_compiled(i).get();
			++out;
			i += 3;
		}
	}
}

void level::load_character(variant c)
{
	chars_.push_back(entity::build(c));
	layers_.insert(chars_.back()->zorder());
	if(!chars_.back()->is_human()) {
		chars_.back()->set_id(chars_.size());
	}
	if(chars_.back()->is_human()) {
#if !defined(__native_client__)
		if(players_.size() == multiplayer::slot()) {
			last_touched_player_ = player_ = chars_.back();
		}
#endif
		ASSERT_LOG(!g_player_type || g_player_type->match(variant(chars_.back().get())), "Player object being added to level does not match required player type. " << chars_.back()->debug_description() << " is not a " << g_player_type->to_string());

		players_.push_back(chars_.back());
		players_.back()->get_player_info()->set_player_slot(players_.size() - 1);
	}

	const int group = chars_.back()->group();
	if(group >= 0) {
		if(group >= groups_.size()) {
			groups_.resize(group + 1);
		}

		groups_[group].push_back(chars_.back());
	}

	if(chars_.back()->label().empty() == false) {
		chars_by_label_[chars_.back()->label()] = chars_.back();
	}

	solid_chars_.clear();
}

PREF_BOOL(respect_difficulty, false, "");

void level::finish_loading()
{
	assert(refcount() > 0);
	current_level_scope level_scope(this);

	std::vector<sub_level_data> sub_levels;
	if((segment_width_ > 0 || segment_height_ > 0) && !editor_ && !preferences::compiling_tiles) {

		const int seg_width = segment_width_ > 0 ? segment_width_ : boundaries_.w();
		const int seg_height = segment_height_ > 0 ? segment_height_ : boundaries_.h();

		for(int y = boundaries_.y(); y < boundaries_.y2(); y += seg_height) {
			for(int x = boundaries_.x(); x < boundaries_.x2(); x += seg_width) {
				level* sub_level = new level(*this);
				const rect bounds(x, y, seg_width, seg_height);

				sub_level->boundaries_ = bounds;
				sub_level->tiles_.erase(std::remove_if(sub_level->tiles_.begin(), sub_level->tiles_.end(), boost::bind(level_tile_not_in_rect, bounds, _1)), sub_level->tiles_.end());
				sub_level->solid_.clear();
				sub_level->standable_.clear();
				foreach(const level_tile& t, sub_level->tiles_) {
					sub_level->add_tile_solid(t);
				}
				sub_level->prepare_tiles_for_drawing();

				sub_level_data data;
				data.lvl.reset(sub_level);
				data.xbase = x;
				data.ybase = y;
				data.xoffset = data.yoffset = 0;
				data.active = false;
				sub_levels.push_back(data);
			}
		}

		const std::vector<entity_ptr> objects = get_chars();
		foreach(const entity_ptr& obj, objects) {
			if(!obj->is_human()) {
				remove_character(obj);
			}
		}

		solid_.clear();
		standable_.clear();
		tiles_.clear();
		prepare_tiles_for_drawing();

		int index = 0;
		foreach(const sub_level_data& data, sub_levels) {
			sub_levels_[formatter() << index] = data;
			++index;
		}
	}

	if(sub_levels_.empty() == false) {
		solid_base_ = solid_;
		standable_base_ = standable_;
	}

	graphics::texture::build_textures_from_worker_threads();

	if (editor_ || preferences::compiling_tiles)
		game_logic::set_verbatim_string_expressions (true);

	std::vector<entity_ptr> objects_not_in_level;

	{
	game_logic::wml_formula_callable_read_scope read_scope;
	foreach(variant node, serialized_objects_) {
		foreach(variant obj_node, node["character"].as_list()) {
			game_logic::wml_serializable_formula_callable_ptr obj;

			std::string addr_str;

			if(obj_node.is_map()) {
				addr_str = obj_node["_addr"].as_string();
				entity_ptr e(entity::build(obj_node));
				objects_not_in_level.push_back(e);
				obj = e;
			} else {
				obj = obj_node.try_convert<game_logic::wml_serializable_formula_callable>();
				addr_str = obj->addr();
			}
			const intptr_t addr_id = strtoll(addr_str.c_str(), NULL, 16);

			game_logic::wml_formula_callable_read_scope::register_serialized_object(addr_id, obj);
		}
	}

	foreach(variant node, wml_chars_) {
		load_character(node);

		const intptr_t addr_id = strtoll(node["_addr"].as_string().c_str(), NULL, 16);
		game_logic::wml_formula_callable_read_scope::register_serialized_object(addr_id, chars_.back());

		if(node.has_key("attached_objects")) {
			std::cerr << "LOADING ATTACHED: " << node["attached_objects"].as_string() << "\n";
			std::vector<entity_ptr> attached;
			std::vector<std::string> v = util::split(node["attached_objects"].as_string());
			foreach(const std::string& s, v) {
				std::cerr << "ATTACHED: " << s << "\n";
				const intptr_t addr_id = strtoll(s.c_str(), NULL, 16);
				game_logic::wml_serializable_formula_callable_ptr obj = game_logic::wml_formula_callable_read_scope::get_serialized_object(addr_id);
				entity* e = dynamic_cast<entity*>(obj.get());
				if(e) {
					std::cerr << "GOT ATTACHED\n";
					attached.push_back(entity_ptr(e));
				}
			}

			chars_.back()->set_attached_objects(attached);
		}
	}

	game_logic::set_verbatim_string_expressions (false);

	wml_chars_.clear();
	serialized_objects_.clear();

	controls::new_level(cycle_, 
		players_.empty() ? 1 : players_.size(), 
#if !defined(__native_client__)
		multiplayer::slot()	
#else
		0
#endif
		);

	//start loading FML for previous and next level
	if(!previous_level().empty()) {
		preload_level_wml(previous_level());
	}

	if(!next_level().empty()) {
		preload_level_wml(next_level());
	}

	if(!sub_levels.empty()) {
		const int seg_width = segment_width_ > 0 ? segment_width_ : boundaries_.w();
		const int seg_height = segment_height_ > 0 ? segment_height_ : boundaries_.h();
		int segment_number = 0;
		for(int y = boundaries_.y(); y < boundaries_.y2(); y += seg_height) {
			for(int x = boundaries_.x(); x < boundaries_.x2(); x += seg_width) {
				const std::vector<entity_ptr> objects = get_chars();
				foreach(const entity_ptr& obj, objects) {
					if(!obj->is_human() && obj->midpoint().x >= x && obj->midpoint().x < x + seg_width && obj->midpoint().y >= y && obj->midpoint().y < y + seg_height) {
						ASSERT_INDEX_INTO_VECTOR(segment_number, sub_levels);
						sub_levels[segment_number].lvl->add_character(obj);
						remove_character(obj);
					}
				}

				++segment_number;
			}
		}
	}

	} //end serialization read scope. Now all objects should be fully resolved.

	if((g_respect_difficulty || preferences::force_difficulty() != INT_MIN) && !editor_) {
		const int difficulty = current_difficulty();
		for(int n = 0; n != chars_.size(); ++n) {
			if(chars_[n].get() != NULL && !chars_[n]->appears_at_difficulty(difficulty)) {
				chars_[n] = entity_ptr();
			}
		}

		chars_.erase(std::remove(chars_.begin(), chars_.end(), entity_ptr()), chars_.end());
	}

#if defined(USE_BOX2D)
	for(std::vector<box2d::body_ptr>::const_iterator it = bodies_.begin(); 
		it != bodies_.end();
		++it) {
		(*it)->finish_loading();
		std::cerr << "level body finish loading: " << std::hex << intptr_t((*it).get()) << " " << intptr_t((*it)->get_raw_body_ptr()) << std::dec << std::endl;
	}
#endif

	//iterate over all our objects and let them do any final loading actions.
	foreach(entity_ptr e, objects_not_in_level) {
		if(e) {
			e->finish_loading(this);
		}
	}

	foreach(entity_ptr e, chars_) {
		if(e) {
			e->finish_loading(this);
		}
	}
/*  Removed firing create_object() for now since create relies on things
    that might not be around yet.
	const std::vector<entity_ptr> chars = chars_;
	foreach(const entity_ptr& e, chars) {
		const bool res = e->create_object();
		if(!res) {
			e->validate_properties();
		}
	}
	*/
}

void level::set_multiplayer_slot(int slot)
{
#if !defined(__native_client__)
	ASSERT_INDEX_INTO_VECTOR(slot, players_);
	last_touched_player_ = player_ = players_[slot];
	controls::new_level(cycle_, players_.empty() ? 1 : players_.size(), slot);
#endif
}

void level::load_save_point(const level& lvl)
{
	if(lvl.save_point_x_ < 0) {
		return;
	}

	save_point_x_ = lvl.save_point_x_;
	save_point_y_ = lvl.save_point_y_;
	if(player_) {
		player_->set_pos(save_point_x_, save_point_y_);
	}
}

namespace {
//we allow rebuilding tiles in the background. We only rebuild the tiles
//one at a time, if more requests for rebuilds come in while we are
//rebuilding, then queue the requests up.

//the level we're currently building tiles for.
const level* level_building = NULL;

struct level_tile_rebuild_info {
	level_tile_rebuild_info() : tile_rebuild_in_progress(false),
	                            tile_rebuild_queued(false),
								rebuild_tile_thread(NULL),
								tile_rebuild_complete(false)
	{}

	//record whether we are currently rebuilding tiles, and if we have had
	//another request come in during the current building of tiles.
	bool tile_rebuild_in_progress;
	bool tile_rebuild_queued;

	threading::thread* rebuild_tile_thread;

	//an unsynchronized buffer only accessed by the main thread with layers
	//that will be rebuilt.
	std::vector<int> rebuild_tile_layers_buffer;

	//buffer accessed by the worker thread which contains layers that will
	//be rebuilt.
	std::vector<int> rebuild_tile_layers_worker_buffer;

	//a locked flag which is polled to see if tile rebuilding has been completed.
	bool tile_rebuild_complete;

	threading::mutex tile_rebuild_complete_mutex;

	//the tiles where the thread will store the new tiles.
	std::vector<level_tile> task_tiles;
};

std::map<const level*, level_tile_rebuild_info> tile_rebuild_map;

void build_tiles_thread_function(level_tile_rebuild_info* info, std::map<int, tile_map> tile_maps, threading::mutex& sync) {
	info->task_tiles.clear();

	if(info->rebuild_tile_layers_worker_buffer.empty()) {
		for(std::map<int, tile_map>::const_iterator i = tile_maps.begin();
		    i != tile_maps.end(); ++i) {
			i->second.build_tiles(&info->task_tiles);
		}
	} else {
		foreach(int layer, info->rebuild_tile_layers_worker_buffer) {
			std::map<int, tile_map>::const_iterator itor = tile_maps.find(layer);
			if(itor != tile_maps.end()) {
				itor->second.build_tiles(&info->task_tiles);
			}
		}
	}

	threading::lock l(info->tile_rebuild_complete_mutex);
	info->tile_rebuild_complete = true;
}

}

void level::start_rebuild_hex_tiles_in_background(const std::vector<int>& layers)
{
	hex_maps_[layers[0]]->build();
}

void level::start_rebuild_tiles_in_background(const std::vector<int>& layers)
{
	level_tile_rebuild_info& info = tile_rebuild_map[this];

	//merge the new layers with any layers we already have queued up.
	if(layers.empty() == false && (!info.tile_rebuild_queued || info.rebuild_tile_layers_buffer.empty() == false)) {
		//add the layers we want to rebuild to those already requested.
		info.rebuild_tile_layers_buffer.insert(info.rebuild_tile_layers_buffer.end(), layers.begin(), layers.end());
		std::sort(info.rebuild_tile_layers_buffer.begin(), info.rebuild_tile_layers_buffer.end());
		info.rebuild_tile_layers_buffer.erase(std::unique(info.rebuild_tile_layers_buffer.begin(), info.rebuild_tile_layers_buffer.end()), info.rebuild_tile_layers_buffer.end());
	} else if(layers.empty()) {
		info.rebuild_tile_layers_buffer.clear();
	}

	if(info.tile_rebuild_in_progress) {
		info.tile_rebuild_queued = true;
		return;
	}

	info.tile_rebuild_in_progress = true;
	info.tile_rebuild_complete = false;

	info.rebuild_tile_layers_worker_buffer = info.rebuild_tile_layers_buffer;
	info.rebuild_tile_layers_buffer.clear();

	std::map<int, tile_map> worker_tile_maps = tile_maps_;
	for(std::map<int, tile_map>::iterator i = worker_tile_maps.begin();
	    i != worker_tile_maps.end(); ++i) {
		//make the tile maps safe to go into a worker thread.
		i->second.prepare_for_copy_to_worker_thread();
	}

	static threading::mutex* sync = new threading::mutex;

	info.rebuild_tile_thread = new threading::thread("rebuild_tiles", boost::bind(build_tiles_thread_function, &info, worker_tile_maps, *sync));
}

void level::freeze_rebuild_tiles_in_background()
{
	level_tile_rebuild_info& info = tile_rebuild_map[this];
	info.tile_rebuild_in_progress = true;
}

void level::unfreeze_rebuild_tiles_in_background()
{
	level_tile_rebuild_info& info = tile_rebuild_map[this];
	if(info.rebuild_tile_thread != NULL) {
		//a thread is actually in flight calculating tiles, so any requests
		//would have been queued up anyway.
		return;
	}

	info.tile_rebuild_in_progress = false;
	start_rebuild_tiles_in_background(info.rebuild_tile_layers_buffer);
}

namespace {
bool level_tile_from_layer(const level_tile& t, int zorder) {
	return t.layer_from == zorder;
}

int g_tile_rebuild_state_id;

}

int level::tile_rebuild_state_id()
{
	return g_tile_rebuild_state_id;
}

void level::set_player_variant_type(variant type_str)
{
	if(type_str.is_null()) {
		type_str = variant("custom_obj");
	}

	using namespace game_logic;

	g_player_type = parse_variant_type(type_str);

	const_formula_callable_definition_ptr def = game_logic::get_formula_callable_definition("level");
	assert(def.get());

	formula_callable_definition* mutable_def = const_cast<formula_callable_definition*>(def.get());
	formula_callable_definition::entry* entry = mutable_def->get_entry_by_id("player");
	assert(entry);
	entry->set_variant_type(g_player_type);
}

void level::complete_rebuild_tiles_in_background()
{
	level_tile_rebuild_info& info = tile_rebuild_map[this];
	if(!info.tile_rebuild_in_progress) {
		return;
	}

	{
		threading::lock l(info.tile_rebuild_complete_mutex);
		if(!info.tile_rebuild_complete) {
			return;
		}
	}

	const int begin_time = SDL_GetTicks();

//	ASSERT_LOG(rebuild_tile_thread, "REBUILD TILE THREAD IS NULL");
	delete info.rebuild_tile_thread;
	info.rebuild_tile_thread = NULL;

	if(info.rebuild_tile_layers_worker_buffer.empty()) {
		tiles_.clear();
	} else {
		foreach(int layer, info.rebuild_tile_layers_worker_buffer) {
			tiles_.erase(std::remove_if(tiles_.begin(), tiles_.end(), boost::bind(level_tile_from_layer, _1, layer)), tiles_.end());
		}
	}

	tiles_.insert(tiles_.end(), info.task_tiles.begin(), info.task_tiles.end());
	info.task_tiles.clear();

	complete_tiles_refresh();

	std::cerr << "COMPLETE TILE REBUILD: " << (SDL_GetTicks() - begin_time) << "\n";

	info.rebuild_tile_layers_worker_buffer.clear();

	info.tile_rebuild_in_progress = false;
	if(info.tile_rebuild_queued) {
		info.tile_rebuild_queued = false;
		start_rebuild_tiles_in_background(info.rebuild_tile_layers_buffer);
	}

	++g_tile_rebuild_state_id;
}

void level::rebuild_tiles()
{
	if(editor_tile_updates_frozen_) {
		return;
	}

	tiles_.clear();
	for(std::map<int, tile_map>::iterator i = tile_maps_.begin(); i != tile_maps_.end(); ++i) {
		i->second.build_tiles(&tiles_);
	}

	complete_tiles_refresh();
}

void level::complete_tiles_refresh()
{
	const int start = SDL_GetTicks();
	std::cerr << "adding solids..." << (SDL_GetTicks() - start) << "\n";
	solid_.clear();
	standable_.clear();

	foreach(level_tile& t, tiles_) {
		add_tile_solid(t);
		layers_.insert(t.zorder);
	}

	std::cerr << "sorting..." << (SDL_GetTicks() - start) << "\n";

	if(std::adjacent_find(tiles_.rbegin(), tiles_.rend(), level_tile_zorder_pos_comparer()) != tiles_.rend()) {
		std::sort(tiles_.begin(), tiles_.end(), level_tile_zorder_pos_comparer());
	}
	prepare_tiles_for_drawing();
	std::cerr << "done..." << (SDL_GetTicks() - start) << "\n";

	const std::vector<entity_ptr> chars = chars_;
	foreach(const entity_ptr& e, chars) {
		e->handle_event("level_tiles_refreshed");
	}
}

int level::variations(int xtile, int ytile) const
{
	for(std::map<int, tile_map>::const_iterator i = tile_maps_.begin();
	    i != tile_maps_.end(); ++i) {
		const int var = i->second.get_variations(xtile, ytile);
		if(var > 1) {
			return var;
		}
	}

	return 1;
}

void level::flip_variations(int xtile, int ytile, int delta)
{
	for(std::map<int, tile_map>::iterator i = tile_maps_.begin();
	    i != tile_maps_.end(); ++i) {
		std::cerr << "get_variations zorder: " << i->first << "\n";
		if(i->second.get_variations(xtile, ytile) > 1) {
			i->second.flip_variation(xtile, ytile, delta);
		}
	}

	rebuild_tiles_rect(rect(xtile*TileSize, ytile*TileSize, TileSize, TileSize));
}

namespace {
struct TileInRect {
	explicit TileInRect(const rect& r) : rect_(r)
	{}

	bool operator()(const level_tile& t) const {
		return point_in_rect(point(t.x, t.y), rect_);
	}

	rect rect_;
};
}

void level::rebuild_tiles_rect(const rect& r)
{
	if(editor_tile_updates_frozen_) {
		return;
	}

	for(int x = r.x(); x < r.x2(); x += TileSize) {
		for(int y = r.y(); y < r.y2(); y += TileSize) {
			tile_pos pos(x/TileSize, y/TileSize);
			solid_.erase(pos);
			standable_.erase(pos);
		}
	}

	tiles_.erase(std::remove_if(tiles_.begin(), tiles_.end(), TileInRect(r)), tiles_.end());

	std::vector<level_tile> tiles;
	for(std::map<int, tile_map>::const_iterator i = tile_maps_.begin(); i != tile_maps_.end(); ++i) {
		i->second.build_tiles(&tiles, &r);
	}

	foreach(level_tile& t, tiles) {
		add_tile_solid(t);
		tiles_.push_back(t);
		layers_.insert(t.zorder);
	}

	if(std::adjacent_find(tiles_.rbegin(), tiles_.rend(), level_tile_zorder_pos_comparer()) != tiles_.rend()) {
		std::sort(tiles_.begin(), tiles_.end(), level_tile_zorder_pos_comparer());
	}
	prepare_tiles_for_drawing();
}

std::string level::package() const
{
	std::string::const_iterator i = std::find(id_.begin(), id_.end(), '/');
	if(i == id_.end()) {
		return "";
	}

	return std::string(id_.begin(), i);
}

variant level::write() const
{
	std::sort(tiles_.begin(), tiles_.end(), level_tile_zorder_pos_comparer());
	game_logic::wml_formula_callable_serialization_scope serialization_scope;

	variant_builder res;
	res.add("id", id_);
	res.add("version", preferences::version());
	res.add("title", title_);
	res.add("music", music_);
	res.add("segment_width", segment_width_);
	res.add("segment_height", segment_height_);

	if(x_resolution_ || y_resolution_) {
		res.add("x_resolution", x_resolution_);
		res.add("y_resolution", y_resolution_);
	}

	res.add("set_screen_resolution_on_entry", set_screen_resolution_on_entry_);

	if(!gui_algo_str_.empty() && !(gui_algo_str_.front() == "default" && gui_algo_str_.size() == 1)) {
		foreach(std::string gui_str, gui_algo_str_) {
			res.add("gui", gui_str);
		}
	}

	if(dark_) {
		res.add("dark", true);
	}

	if(dark_color_.to_string() != default_dark_color().to_string()) {
		res.add("dark_color", dark_color_.write());
	}

	if(cycle_) {
		res.add("cycle", cycle_);
	}

	if(!sub_level_str_.empty()) {
		res.add("sub_levels", sub_level_str_);
	}

	res.add("dimensions", boundaries().write());

	res.add("xscale", xscale_);
	res.add("yscale", yscale_);
	res.add("auto_move_camera", auto_move_camera_.write());
	res.add("air_resistance", air_resistance_);
	res.add("water_resistance", water_resistance_);

	res.add("touch_controls", allow_touch_controls_);

	res.add("preloads", util::join(preloads_));

	if(lock_screen_) {
		res.add("lock_screen", lock_screen_->to_string());
	}

	if(water_) {
		res.add("water", water_->write());
	}

	if(camera_rotation_) {
		res.add("camera_rotation", camera_rotation_->str());
	}

	foreach(const solid_rect& r, solid_rects_) {
		variant_builder node;
		node.add("rect", r.r.write());
		node.add("friction", r.friction);
		node.add("traction", r.traction);
		node.add("damage", r.damage);

		res.add("solid_rect", node.build());
	}

	for(std::map<int,hex::hex_map_ptr>::const_iterator i = hex_maps_.begin(); i != hex_maps_.end(); ++i) {
		res.add("hex_tile_map", i->second->write());
	}

	for(std::map<int, tile_map>::const_iterator i = tile_maps_.begin(); i != tile_maps_.end(); ++i) {
		variant node = i->second.write();
		if(preferences::compiling_tiles) {
			node.add_attr(variant("tiles"), variant(""));
			node.add_attr(variant("unique_tiles"), variant(""));
		}
		res.add("tile_map", node);
	}

	if(preferences::compiling_tiles && !tiles_.empty()) {

		level_object::set_current_palette(palettes_used_);

		int num_tiles = 0;
		int last_zorder = INT_MIN;
		int basex = 0, basey = 0;
		int last_x = 0, last_y = 0;
		std::string tiles_str;
		for(int n = 0; n <= tiles_.size(); ++n) {
			if(n != tiles_.size() && tiles_[n].draw_disabled && tiles_[n].object->has_solid() == false) {
				continue;
			}

			if(n == tiles_.size() || tiles_[n].zorder != last_zorder) {
				if(!tiles_str.empty()) {
					variant_builder node;
					node.add("zorder", write_zorder(last_zorder));
					node.add("x", basex);
					node.add("y", basey);
					node.add("tiles", tiles_str);
					res.add("compiled_tiles", node.build());
				}

				if(n == tiles_.size()) {
					break;
				}

				tiles_str.clear();

				last_zorder = tiles_[n].zorder;

				basex = basey = INT_MAX;
				for(int m = n; m != tiles_.size() && tiles_[m].zorder == tiles_[n].zorder; ++m) {
					if(tiles_[m].x < basex) {
						basex = tiles_[m].x;
					}

					if(tiles_[m].y < basey) {
						basey = tiles_[m].y;
					}
				}

				last_x = basex;
				last_y = basey;
			}

			while(last_y < tiles_[n].y) {
				tiles_str += "\n";
				last_y += TileSize;
				last_x = basex;
			}

			while(last_x < tiles_[n].x) {
				tiles_str += ",";
				last_x += TileSize;
			}

			ASSERT_EQ(last_x, tiles_[n].x);
			ASSERT_EQ(last_y, tiles_[n].y);

			if(tiles_[n].face_right) {
				tiles_str += "~";
			}

			const int xpos = tiles_[n].x;
			const int ypos = tiles_[n].y;
			const int zpos = tiles_[n].zorder;
			const int start_n = n;

			while(n != tiles_.size() && tiles_[n].x == xpos && tiles_[n].y == ypos && tiles_[n].zorder == zpos) {
				char buf[4];
				tiles_[n].object->write_compiled_index(buf);
				if(n != start_n) {
					tiles_str += "|";
				}
				tiles_str += buf;
				++n;
				++num_tiles;
			}

			--n;

			tiles_str += ",";

			last_x += TileSize;
		}

		res.add("num_compiled_tiles", num_tiles);

		//calculate rectangular opaque areas of tiles that allow us
		//to avoid drawing the background. Start by calculating the set
		//of tiles that are opaque.
		typedef std::pair<int,int> OpaqueLoc;
		std::set<OpaqueLoc> opaque;
		foreach(const level_tile& t, tiles_) {
			if(t.object->is_opaque() == false) {
				continue;
			}

			std::map<int, tile_map>::const_iterator tile_itor = tile_maps_.find(t.zorder);
			ASSERT_LOG(tile_itor != tile_maps_.end(), "COULD NOT FIND TILE LAYER IN MAP");
			if(tile_itor->second.x_speed() != 100 || tile_itor->second.y_speed() != 100) {
				//we only consider the layer that moves at 100% speed,
				//since calculating obscured areas at other layers is too
				//complicated.
				continue;
			}

			opaque.insert(std::pair<int,int>(t.x,t.y));
		}

		std::cerr << "BUILDING RECTS...\n";

		std::vector<rect> opaque_rects;

		//keep iterating, finding the largest rectangle we can make of
		//available opaque locations, then removing all those opaque
		//locations from our set, until we have all areas covered.
		while(!opaque.empty()) {
			rect largest_rect;

			//iterate over every opaque location, treating each one
			//as a possible upper-left corner of our rectangle.
			for(std::set<OpaqueLoc>::const_iterator loc_itor = opaque.begin();
			    loc_itor != opaque.end(); ++loc_itor) {
				const OpaqueLoc& loc = *loc_itor;

				std::vector<OpaqueLoc> v;
				v.push_back(loc);

				std::set<OpaqueLoc>::const_iterator find_itor = opaque.end();

				int prev_rows = 0;

				//try to build a top row of a rectangle. After adding each
				//cell, we will try to expand the rectangle downwards, as
				//far as it will go.
				while((find_itor = opaque.find(OpaqueLoc(v.back().first + TileSize, v.back().second))) != opaque.end()) {
					v.push_back(OpaqueLoc(v.back().first + TileSize, v.back().second));

					int rows = 1;

					bool found_non_opaque = false;
					while(found_non_opaque == false) {
						for(int n = rows < prev_rows ? v.size()-1 : 0; n != v.size(); ++n) {
							if(!opaque.count(OpaqueLoc(v[n].first, v[n].second + rows*TileSize))) {
								found_non_opaque = true;
								break;
							}
						}

						if(found_non_opaque == false) {
							++rows;
						}
					}

					prev_rows = rows;

					rect r(v.front().first, v.front().second, v.size()*TileSize, rows*TileSize);
					if(r.w()*r.h() > largest_rect.w()*largest_rect.h()) {
						largest_rect = r;
					}
				} //end while expand rectangle to the right.
			} //end for iterating over all possible rectangle upper-left positions

			std::cerr << "LARGEST_RECT: " << largest_rect.w() << " x " << largest_rect.h() << "\n";

			//have a minimum size for rectangles. If we fail to reach
			//the minimum size then just stop. It's not worth bothering 
			//with lots of small little rectangles.
			if(largest_rect.w()*largest_rect.h() < TileSize*TileSize*32) {
				break;
			}

			opaque_rects.push_back(largest_rect);

			for(std::set<OpaqueLoc>::iterator i = opaque.begin();
			    i != opaque.end(); ) {
				if(i->first >= largest_rect.x() && i->second >= largest_rect.y() && i->first < largest_rect.x2() && i->second < largest_rect.y2()) {
					opaque.erase(i++);
				} else {
					++i;
				}
			}
		} //end searching for rectangles to add.
		std::cerr << "DONE BUILDING RECTS...\n";

		if(!opaque_rects.empty()) {
			std::ostringstream opaque_rects_str;
			foreach(const rect& r, opaque_rects) {
				opaque_rects_str << r.to_string() << ":";
			}

			res.add("opaque_rects", opaque_rects_str.str());

			std::cerr << "RECTS: " << id_ << ": " << opaque_rects.size() << "\n";
		}
	} //end if preferences::compiling

	foreach(entity_ptr ch, chars_) {
		if(!ch->serializable()) {
			continue;
		}

		variant node(ch->write());
		game_logic::wml_formula_callable_serialization_scope::register_serialized_object(ch);
		res.add("character", node);
	}

	foreach(const portal& p, portals_) {
		variant_builder node;
		node.add("rect", p.area.write());
		node.add("level", p.level_dest);
		node.add("dest_starting_pos", p.dest_starting_pos);
		node.add("dest", p.dest.to_string());
		node.add("automatic", p.automatic);
		node.add("transition", p.transition);
		res.add("portal", node.build());
	}

	if(right_portal_.level_dest.empty() == false) {
		res.add("next_level", right_portal_.level_dest);
	}

	std::cerr << "PREVIOUS LEVEL: " << left_portal_.level_dest << "\n";
	if(left_portal_.level_dest.empty() == false) {
		res.add("previous_level", left_portal_.level_dest);
	}

	if(background_) {
		if(background_->id().empty()) {
			res.add("background", background_->write());
		} else {
			res.add("background", background_->id());
			res.add("background_offset", background_offset_.write());
		}
	}

	for(std::map<std::string, movement_script>::const_iterator i = movement_scripts_.begin(); i != movement_scripts_.end(); ++i) {
		res.add("script", i->second.write());
	}

	if(num_compiled_tiles_ > 0) {
		res.add("num_compiled_tiles", num_compiled_tiles_);
		foreach(variant compiled_node, wml_compiled_tiles_) {
			res.add("compiled_tiles", compiled_node);
		}
	}

	if(palettes_used_) {
		std::vector<variant> out;
		unsigned int p = palettes_used_;
		int id = 0;
		while(p) {
			if(p&1) {
				out.push_back(variant(graphics::get_palette_name(id)));
			}

			p >>= 1;
			++id;
		}

		res.add("palettes", variant(&out));
	}

	if(background_palette_ != -1) {
		res.add("background_palette", graphics::get_palette_name(background_palette_));
	}

	res.add("vars", vars_);

#if defined(USE_SHADERS)
	if(shader_) {
		res.add("shader", shader_->write());
	}
#endif

#if defined(USE_ISOMAP)
	if(iso_world_) {
		res.add("isoworld", iso_world_->write());
	}
	if(camera_) {
		res.add("camera", camera_->write());
	}
#endif

#if defined(USE_BOX2D)
	for(std::vector<box2d::body_ptr>::const_iterator it = bodies_.begin(); 
		it != bodies_.end();
		++it) {
		res.add("bodies", (*it)->write());
	}
#endif

	variant result = res.build();
	result.add_attr(variant("serialized_objects"), serialization_scope.write_objects(result));
	return result;
}

point level::get_dest_from_str(const std::string& key) const
{
	int ypos = 0;
	if(player()) {
		ypos = player()->get_entity().y();
	}
	if(key == "left") {
		return point(boundaries().x() + 32, ypos);
	} else if(key == "right") {
		return point(boundaries().x2() - 128, ypos);
	} else {
		return point();
	}
}

const std::string& level::previous_level() const
{
	return left_portal_.level_dest;
}

const std::string& level::next_level() const
{
	return right_portal_.level_dest;
}

void level::set_previous_level(const std::string& name)
{
	left_portal_.level_dest = name;
	left_portal_.dest_str = "right";
	left_portal_.dest_starting_pos = false;
	left_portal_.automatic = true;
}

void level::set_next_level(const std::string& name)
{
	right_portal_.level_dest = name;
	right_portal_.dest_str = "left";
	right_portal_.dest_starting_pos = false;
	right_portal_.automatic = true;
}

namespace {
//counter incremented every time the level is drawn.
int draw_count = 0;
}

void level::draw_layer(int layer, int x, int y, int w, int h) const
{
	if(layer >= 1000 && editor_ && show_foreground_ == false) {
		return;
	}

	for(std::map<std::string, sub_level_data>::const_iterator i = sub_levels_.begin(); i != sub_levels_.end(); ++i) {
		if(i->second.active) {
			glPushMatrix();
			glTranslatef(i->second.xoffset, GLfloat(i->second.yoffset), 0.0);
			i->second.lvl->draw_layer(layer, x - i->second.xoffset, y - i->second.yoffset - TileSize, w, h + TileSize);
			glPopMatrix();
		}
	}

	if(editor_ && layer == highlight_layer_) {
		const GLfloat alpha = GLfloat(0.3 + (1.0+sin(draw_count/5.0))*0.35);
		glColor4f(1.0, 1.0, 1.0, alpha);

	} else if(editor_ && hidden_layers_.count(layer)) {
		glColor4f(1.0, 1.0, 1.0, 0.3);
	}

	glPushMatrix();

	graphics::distortion_translation distort_translation;
	
	// parallax scrolling for tiles.
	std::map<int, tile_map>::const_iterator tile_map_iterator = tile_maps_.find(layer);
	if(tile_map_iterator != tile_maps_.end()) {
		int scrollx = tile_map_iterator->second.x_speed();
		int scrolly = tile_map_iterator->second.y_speed();

		const int diffx = ((scrollx - 100)*x)/100;
		const int diffy = ((scrolly - 100)*y)/100;

		glTranslatef(diffx, diffy, 0.0);
		distort_translation.translate(diffx, diffy);
		
		//here, we adjust the screen bounds (they're a first order optimization) to account for the parallax shift
		x -= diffx;
		y -= diffy;
	} 

	typedef std::vector<level_tile>::const_iterator itor;
	std::pair<itor,itor> range = std::equal_range(tiles_.begin(), tiles_.end(), layer, level_tile_zorder_comparer());

	itor tile_itor = std::lower_bound(range.first, range.second, y,
	                          level_tile_y_pos_comparer());

	if(tile_itor == range.second) {
		glPopMatrix();
		return;
	}

	std::map<int, layer_blit_info>::iterator layer_itor = blit_cache_.find(layer);
	if(layer_itor == blit_cache_.end()) {
		glPopMatrix();
		return;
	}

	const level_tile* t = &*tile_itor;
	const level_tile* end_tiles = &*tiles_.begin() + tiles_.size();

	layer_blit_info& blit_info = layer_itor->second;

	const rect tile_positions(x/TileSize - (x < 0 ? 1 : 0), y/TileSize - (y < 0 ? 1 : 0),
	                          (x + w)/TileSize - (x + w < 0 ? 1 : 0),
							  (y + h)/TileSize - (y + h < 0 ? 1 : 0));

	std::vector<layer_blit_info::IndexType>& opaque_indexes = blit_info.opaque_indexes;
	std::vector<layer_blit_info::IndexType>& translucent_indexes = blit_info.translucent_indexes;

	if(blit_info.tile_positions != tile_positions || editor_) {
		blit_info.tile_positions = tile_positions;

		opaque_indexes.clear();
		translucent_indexes.clear();

		int ystart = std::max<int>(0, (y - blit_info.ybase)/TileSize);
		int yend = std::min<int>(blit_info.indexes.size(), (y + h - blit_info.ybase)/TileSize + 1);

		for(; ystart < yend; ++ystart) {
			const std::vector<layer_blit_info::IndexType>& indexes = blit_info.indexes[ystart];
			int xstart = std::max<int>(0, (x - blit_info.xbase)/TileSize);
			int xend = std::min<int>(indexes.size(), (x + w - blit_info.xbase)/TileSize + 1);
			for(; xstart < xend; ++xstart) {
				if(indexes[xstart] != TILE_INDEX_TYPE_MAX) {
					if(indexes[xstart] > 0) {
						GLint index = indexes[xstart];
						opaque_indexes.push_back(index);
						opaque_indexes.push_back(index+1);
						opaque_indexes.push_back(index+2);
						opaque_indexes.push_back(index+1);
						opaque_indexes.push_back(index+2);
						opaque_indexes.push_back(index+3);
						ASSERT_INDEX_INTO_VECTOR(index, blit_info.blit_vertexes);
						ASSERT_INDEX_INTO_VECTOR(index+3, blit_info.blit_vertexes);
					} else {
						GLint index = -indexes[xstart];
						translucent_indexes.push_back(index);
						translucent_indexes.push_back(index+1);
						translucent_indexes.push_back(index+2);
						translucent_indexes.push_back(index+1);
						translucent_indexes.push_back(index+2);
						translucent_indexes.push_back(index+3);
						ASSERT_INDEX_INTO_VECTOR(index, blit_info.blit_vertexes);
						ASSERT_INDEX_INTO_VECTOR(index+3, blit_info.blit_vertexes);
					}
				}
			}
		}
	}

	glDisable(GL_BLEND);
	draw_layer_solid(layer, x, y, w, h);
	if(blit_info.texture_id != GLuint(-1)) {
		graphics::texture::set_current_texture(blit_info.texture_id);
	}

#if defined(USE_SHADERS)
	gles2::active_shader()->prepare_draw();
#endif

	if(!opaque_indexes.empty()) {
#if defined(USE_SHADERS)
		gles2::active_shader()->shader()->vertex_array(2, GL_SHORT, GL_FALSE, sizeof(tile_corner), &blit_info.blit_vertexes[0].vertex[0]);
		gles2::active_shader()->shader()->texture_array(2, GL_FLOAT, GL_FALSE, sizeof(tile_corner), &blit_info.blit_vertexes[0].uv[0]);
#else
		glVertexPointer(2, GL_SHORT, sizeof(tile_corner), &blit_info.blit_vertexes[0].vertex[0]);
		glTexCoordPointer(2, GL_FLOAT, sizeof(tile_corner), &blit_info.blit_vertexes[0].uv[0]);
#endif
		glDrawElements(GL_TRIANGLES, opaque_indexes.size(), TILE_INDEX_TYPE, &opaque_indexes[0]);
	}
	glEnable(GL_BLEND);

	if(!translucent_indexes.empty()) {
#if defined(USE_SHADERS)
		gles2::active_shader()->shader()->vertex_array(2, GL_SHORT, GL_FALSE, sizeof(tile_corner), &blit_info.blit_vertexes[0].vertex[0]);
		gles2::active_shader()->shader()->texture_array(2, GL_FLOAT, GL_FALSE, sizeof(tile_corner), &blit_info.blit_vertexes[0].uv[0]);
#else
		glVertexPointer(2, GL_SHORT, sizeof(tile_corner), &blit_info.blit_vertexes[0].vertex[0]);
		glTexCoordPointer(2, GL_FLOAT, sizeof(tile_corner), &blit_info.blit_vertexes[0].uv[0]);
#endif
		if(blit_info.texture_id == GLuint(-1)) {
			//we have multiple different texture ID's in this layer. This means
			//we will draw each tile seperately.
			for(int n = 0; n < translucent_indexes.size(); n += 6) {
				graphics::texture::set_current_texture(blit_info.vertex_texture_ids[translucent_indexes[n]/4]);
				glDrawElements(GL_TRIANGLES, 6, TILE_INDEX_TYPE, &translucent_indexes[n]);
			}
		} else {
			//we have just one texture ID and so can draw all tiles in one call.
			glDrawElements(GL_TRIANGLES, translucent_indexes.size(), TILE_INDEX_TYPE, &translucent_indexes[0]);
		}
	}

	glPopMatrix();

	glColor4f(1.0, 1.0, 1.0, 1.0);
}

void level::draw_layer_solid(int layer, int x, int y, int w, int h) const
{
	solid_color_rect arg;
	arg.layer = layer;
	typedef std::vector<solid_color_rect>::const_iterator SolidItor;
	std::pair<SolidItor, SolidItor> solid = std::equal_range(solid_color_rects_.begin(), solid_color_rects_.end(), arg, solid_color_rect_cmp());
	if(solid.first != solid.second) {
		const rect viewport(x, y, w, h);

#if !defined(USE_SHADERS)
		glDisable(GL_TEXTURE_2D);
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
#endif
		while(solid.first != solid.second) {
			rect area = solid.first->area;
			if(!rects_intersect(area, viewport)) {
				++solid.first;
				continue;
			}

			area = intersection_rect(area, viewport);

			solid.first->color.set_as_current_color();
			GLshort varray[] = {
			  GLshort(area.x()), GLshort(area.y()),
			  GLshort(area.x() + area.w()), GLshort(area.y()),
			  GLshort(area.x()), GLshort(area.y() + area.h()),
			  GLshort(area.x() + area.w()), GLshort(area.y() + area.h()),
			};
#if defined(USE_SHADERS)
			gles2::manager gles2_manager(gles2::get_simple_shader());
			gles2::active_shader()->shader()->vertex_array(2, GL_FLOAT, 0, 0, varray);
#else
			glVertexPointer(2, GL_SHORT, 0, varray);
#endif
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

			++solid.first;
		}
#if !defined(USE_SHADERS)
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		glEnable(GL_TEXTURE_2D);
#endif
		glColor4ub(255, 255, 255, 255);
	}
}

void level::prepare_tiles_for_drawing()
{
	level_object::set_current_palette(palettes_used_);

	solid_color_rects_.clear();
	blit_cache_.clear();

	for(int n = 0; n != tiles_.size(); ++n) {
		if(!is_arcade_level() && tiles_[n].object->solid_color()) {
			continue;
		}

		//in the editor we want to draw the whole level, so don't exclude
		//things outside the level bounds.
		if(!editor_ && (tiles_[n].x <= boundaries().x() - TileSize || tiles_[n].y <= boundaries().y() - TileSize || tiles_[n].x >= boundaries().x2() || tiles_[n].y >= boundaries().y2())) {
			continue;
		}

		layer_blit_info& blit_info = blit_cache_[tiles_[n].zorder];
		if(blit_info.xbase == -1) {
			blit_info.texture_id = tiles_[n].object->texture().get_id();
			blit_info.xbase = tiles_[n].x;
			blit_info.ybase = tiles_[n].y;
		}

		if(tiles_[n].x < blit_info.xbase) {
			blit_info.xbase = tiles_[n].x;
		}

		if(tiles_[n].y < blit_info.ybase) {
			blit_info.ybase = tiles_[n].y;
		}
	}

	for(int n = 0; n != tiles_.size(); ++n) {
		if(!editor_ && (tiles_[n].x <= boundaries().x() - TileSize || tiles_[n].y <= boundaries().y() - TileSize || tiles_[n].x >= boundaries().x2() || tiles_[n].y >= boundaries().y2())) {
			continue;
		}

		if(!is_arcade_level() && tiles_[n].object->solid_color()) {
			tiles_[n].draw_disabled = true;
			if(!solid_color_rects_.empty()) {
				solid_color_rect& r = solid_color_rects_.back();
				if(r.layer == tiles_[n].zorder && r.color.rgba() == tiles_[n].object->solid_color()->rgba() && r.area.y() == tiles_[n].y && r.area.x() + r.area.w() == tiles_[n].x) {
					r.area = rect(r.area.x(), r.area.y(), r.area.w() + TileSize, r.area.h());
					continue;
				}
			}
				
			solid_color_rect r;
			r.color = *tiles_[n].object->solid_color();
			r.area = rect(tiles_[n].x, tiles_[n].y, TileSize, TileSize);
			r.layer = tiles_[n].zorder;
			solid_color_rects_.push_back(r);
			continue;
		}

		layer_blit_info& blit_info = blit_cache_[tiles_[n].zorder];

		tiles_[n].draw_disabled = false;

		blit_info.blit_vertexes.resize(blit_info.blit_vertexes.size() + 4);
		const int npoints = level_object::calculate_tile_corners(&blit_info.blit_vertexes[blit_info.blit_vertexes.size() - 4], tiles_[n]);
		if(npoints == 0) {
			blit_info.blit_vertexes.resize(blit_info.blit_vertexes.size() - 4);
		} else {
			blit_info.vertex_texture_ids.push_back(tiles_[n].object->texture().get_id());
			if(blit_info.vertex_texture_ids.back() != blit_info.texture_id) {
				blit_info.texture_id = GLuint(-1);
			}

			const int xtile = (tiles_[n].x - blit_info.xbase)/TileSize;
			const int ytile = (tiles_[n].y - blit_info.ybase)/TileSize;
			ASSERT_GE(xtile, 0);
			ASSERT_GE(ytile, 0);
			if(blit_info.indexes.size() <= ytile) {
				blit_info.indexes.resize(ytile+1);
			}

			if(blit_info.indexes[ytile].size() <= xtile) {
				blit_info.indexes[ytile].resize(xtile+1, TILE_INDEX_TYPE_MAX);
			}

			blit_info.indexes[ytile][xtile] = (blit_info.blit_vertexes.size() - 4) * (tiles_[n].object->is_opaque() ? 1 : -1);
		}
	}

	for(int n = 1; n < solid_color_rects_.size(); ++n) {
		solid_color_rect& a = solid_color_rects_[n-1];
		solid_color_rect& b = solid_color_rects_[n];
		if(a.area.x() == b.area.x() && a.area.x2() == b.area.x2() && a.area.y() + a.area.h() == b.area.y() && a.layer == b.layer) {
			a.area = rect(a.area.x(), a.area.y(), a.area.w(), a.area.h() + b.area.h());
			b.area = rect(0,0,0,0);
		}
	}

	solid_color_rects_.erase(std::remove_if(solid_color_rects_.begin(), solid_color_rects_.end(), solid_color_rect_empty()), solid_color_rects_.end());

	//remove tiles that are obscured by other tiles.
	std::set<std::pair<int, int> > opaque;
	for(int n = tiles_.size(); n > 0; --n) {
		level_tile& t = tiles_[n-1];
		const tile_map& map = tile_maps_[t.zorder];
		if(map.x_speed() != 100 || map.y_speed() != 100) {
			while(n != 0 && tiles_[n-1].zorder == t.zorder) {
				--n;
			}

			continue;
		}

		if(!t.draw_disabled && opaque.count(std::pair<int,int>(t.x, t.y))) {
			t.draw_disabled = true;
			continue;
		}

		if(t.object->is_opaque()) {
			opaque.insert(std::pair<int,int>(t.x, t.y));
		}
	}

}

void level::draw_status() const
{
	if(!gui_algorithm_.empty()) {
		foreach(gui_algorithm_ptr g, gui_algorithm_) {
			g->draw(*this);
		}
		if(preferences::no_iphone_controls() == false && level::current().allow_touch_controls() == true) {
			iphone_controls::draw();
		}
	}

	if(current_speech_dialog()) {
		current_speech_dialog()->draw();
	}
}

namespace {
void draw_entity(const entity& obj, int x, int y, bool editor) {
	const std::pair<int,int>* scroll_speed = obj.parallax_scale_millis();

	if(scroll_speed) {
		glPushMatrix();
		const int scrollx = scroll_speed->first;
		const int scrolly = scroll_speed->second;

		const int diffx = ((scrollx - 1000)*x)/1000;
		const int diffy = ((scrolly - 1000)*y)/1000;

		glTranslatef(diffx, diffy, 0.0);
	}

	obj.draw(x, y);
	if(editor) {
		obj.draw_group();
	}

	if(scroll_speed) {
		glPopMatrix();
	}
}
void draw_entity_later(const entity& obj, int x, int y, bool editor) {
	const std::pair<int,int>* scroll_speed = obj.parallax_scale_millis();

	if(scroll_speed) {
		glPushMatrix();
		const int scrollx = scroll_speed->first;
		const int scrolly = scroll_speed->second;

		const int diffx = ((scrollx - 1000)*x)/1000;
		const int diffy = ((scrolly - 1000)*y)/1000;

		glTranslatef(diffx, diffy, 0.0);
	}

	obj.draw_later(x, y);

	if(scroll_speed) {
		glPopMatrix();
	}
}
}

extern std::vector<rect> background_rects_drawn;

void level::draw_later(int x, int y, int w, int h) const
{
	// Delayed drawing for some elements.
#if defined(USE_SHADERS)
	gles2::manager manager(shader_);
#endif
	std::vector<entity_ptr>::const_iterator entity_itor = active_chars_.begin();
	while(entity_itor != active_chars_.end()) {
		draw_entity_later(**entity_itor, x, y, editor_);
		++entity_itor;
	}
}


void level::draw(int x, int y, int w, int h) const
{
	sound::process();

	++draw_count;

	const int start_x = x;
	const int start_y = y;
	const int start_w = w;
	const int start_h = h;

	const int ticks = SDL_GetTicks();
	
	x -= widest_tile_;
	y -= highest_tile_;
	w += widest_tile_;
	h += highest_tile_;
	
#if defined(USE_ISOMAP)
	if(iso_world_) {
		// XX hackity hack
		gles2::shader_program_ptr active = gles2::active_shader();
		iso_world_->draw(camera_);
		glUseProgram(active->shader()->get());
	}
#endif

	{
#if defined(USE_SHADERS)
	gles2::manager manager(shader_);
#endif

	std::sort(active_chars_.begin(), active_chars_.end(), zorder_compare);

	const std::vector<entity_ptr>* chars_ptr = &active_chars_;
	std::vector<entity_ptr> editor_chars_buf;

	std::map<int, hex::hex_map_ptr>::const_iterator hit = hex_maps_.begin();
	while(hit != hex_maps_.end()) {
		hit->second->draw();
		++hit;
	}

	if(editor_) {
		editor_chars_buf = active_chars_;
		rect screen_area(x, y, w, h);

		//in the editor draw all characters that are on screen as well
		//as active ones.
		foreach(const entity_ptr& c, chars_) {
			if(std::find(editor_chars_buf.begin(), editor_chars_buf.end(), c) != editor_chars_buf.end()) {
				continue;
			}

			if(std::find(active_chars_.begin(), active_chars_.end(), c) != active_chars_.end() || rects_intersect(c->draw_rect(), screen_area)) {
				editor_chars_buf.push_back(c);
			}
		}

		std::sort(editor_chars_buf.begin(), editor_chars_buf.end(), zorder_compare);
		chars_ptr = &editor_chars_buf;
	}
	const std::vector<entity_ptr>& chars = *chars_ptr;

	std::vector<entity_ptr>::const_iterator entity_itor = chars.begin();

	
	/*std::cerr << "SUMMARY " << cycle_ << ": ";
	foreach(const entity_ptr& e, chars_) {
		std::cerr << e->debug_description() << "(" << e->zsub_order() << "):";
	}
	
	std::cerr << "\n";*/
	
	bool water_drawn = true;
	int water_zorder = 0;
	if(water_) {
		water_drawn = false;
		water_zorder = water_->zorder();
	}

	graphics::stencil_scope stencil_settings(true, 0x02, GL_ALWAYS, 0x02, 0xFF, GL_KEEP, GL_KEEP, GL_REPLACE);
	glClear(GL_STENCIL_BUFFER_BIT);

#ifdef USE_SHADERS
	frame_buffer_enter_zorder(-100000);
	const int begin_alpha_test = get_named_zorder("anura_begin_shadow_casting");
	const int end_alpha_test = get_named_zorder("shadows");
#endif

	std::set<int>::const_iterator layer = layers_.begin();

	for(; layer != layers_.end(); ++layer) {
#ifdef USE_SHADERS
		frame_buffer_enter_zorder(*layer);
		const bool alpha_test = *layer >= begin_alpha_test && *layer < end_alpha_test;
		gles2::set_alpha_test(alpha_test);
		glStencilMask(alpha_test ? 0x02 : 0x0);
#endif
		if(!water_drawn && *layer > water_zorder) {
			water_->draw(x, y, w, h);
			water_drawn = true;
		}

		while(entity_itor != chars.end() && (*entity_itor)->zorder() <= *layer) {
			draw_entity(**entity_itor, x, y, editor_);
			++entity_itor;
		}

		draw_layer(*layer, x, y, w, h);
	}

	if(!water_drawn) {
		water_->draw(x, y, w, h);
			water_drawn = true;
	}

	int last_zorder = -1000000;
	while(entity_itor != chars.end()) {
#ifdef USE_SHADERS
		if((*entity_itor)->zorder() != last_zorder) {
			last_zorder = (*entity_itor)->zorder();
			frame_buffer_enter_zorder(last_zorder);
			const bool alpha_test = last_zorder >= begin_alpha_test && last_zorder < end_alpha_test;
			gles2::set_alpha_test(alpha_test);
			glStencilMask(alpha_test ? 0x02 : 0x0);
		}
#endif

		draw_entity(**entity_itor, x, y, editor_);
		++entity_itor;
	}

#ifdef USE_SHADERS
	gles2::set_alpha_test(false);
	frame_buffer_enter_zorder(1000000);
#endif

	if(editor_) {
		foreach(const entity_ptr& obj, chars_) {
			if(!obj->allow_level_collisions() && entity_collides_with_level(*this, *obj, MOVE_NONE)) {
				//if the entity is colliding with the level, then draw
				//it in red to mark as 'bad'.
				glBlendFunc(GL_SRC_ALPHA, GL_ONE);
				const GLfloat alpha = 0.5 + sin(draw_count/5.0)*0.5;
				glColor4f(1.0, 0.0, 0.0, alpha);
				obj->draw(x, y);
				glColor4f(1.0, 1.0, 1.0, 1.0);
				glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			}
		}
	}

	if(editor_highlight_ || !editor_selection_.empty()) {
		if(editor_highlight_ && std::count(chars_.begin(), chars_.end(), editor_highlight_)) {
			draw_entity(*editor_highlight_, x, y, true);
		}

		foreach(const entity_ptr& e, editor_selection_) {
			if(std::count(chars_.begin(), chars_.end(), e)) {
				draw_entity(*e, x, y, true);
			}
		}

		glBlendFunc(GL_SRC_ALPHA, GL_ONE);
		const GLfloat alpha = 0.5 + sin(draw_count/5.0)*0.5;
		glColor4f(1.0, 1.0, 1.0, alpha);

		if(editor_highlight_ && std::count(chars_.begin(), chars_.end(), editor_highlight_)) {
			if(editor_highlight_->spawned_by().empty() == false) {
				glColor4f(1.0, 1.0, 0.0, alpha);
			}

			draw_entity(*editor_highlight_, x, y, true);
			glColor4f(1.0, 1.0, 1.0, alpha);
		}

		foreach(const entity_ptr& e, editor_selection_) {
			if(std::count(chars_.begin(), chars_.end(), e)) {
				draw_entity(*e, x, y, true);
			}
		}

		glColor4f(1.0, 1.0, 1.0, 1.0);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}

	draw_debug_solid(x, y, w, h);

	if(background_) {
		background_->draw_foreground(start_x, start_y, 0.0, cycle());
	}
	}

	{
#if defined(USE_SHADERS)
	gles2::manager manager(shader_);
#endif
	calculate_lighting(start_x, start_y, start_w, start_h);
	}

	if(g_debug_shadows) {
		graphics::stencil_scope scope(true, 0x0, GL_EQUAL, 0x02, 0xFF, GL_KEEP, GL_KEEP, GL_KEEP);
		graphics::draw_rect(rect(x,y,w,h), graphics::color(255, 255, 255, 196 + sin(SDL_GetTicks()/100.0)*8.0));
	}
}

#ifdef USE_SHADERS
void level::frame_buffer_enter_zorder(int zorder) const
{
	std::vector<gles2::shader_program_ptr> shaders;
	foreach(const FrameBufferShaderEntry& e, fb_shaders_) {
		if(zorder >= e.begin_zorder && zorder <= e.end_zorder) {
			if(!e.shader) {
				if(e.shader_node.is_string()) {
					e.shader = gles2::shader_program::get_global(e.shader_node.as_string());
				} else {
					e.shader.reset(new gles2::shader_program(e.shader_node));
				}
			}

			shaders.push_back(e.shader);
		}
	}

	if(shaders != active_fb_shaders_) {
		if(active_fb_shaders_.empty()) {
			texture_frame_buffer::set_render_to_texture();
			glClearColor(0.0, 0.0, 0.0, 0.0);
			glClear(GL_COLOR_BUFFER_BIT);
		} else if(shaders.empty()) {
			//now there are no shaders, flush all to the screen and proceed with
			//rendering to the screen.
			flush_frame_buffer_shaders_to_screen();
			texture_frame_buffer::set_render_to_screen();
		} else {
			bool add_shaders = false;
			foreach(const gles2::shader_program_ptr& s, shaders) {
				if(std::count(active_fb_shaders_.begin(), active_fb_shaders_.end(), s) == 0) {
					add_shaders = true;
					break;
				}
			}

			if(add_shaders) {
				//this works if we're adding and removing shaders.
				flush_frame_buffer_shaders_to_screen();
				texture_frame_buffer::set_render_to_texture();
				glClearColor(0.0, 0.0, 0.0, 0.0);
				glClear(GL_COLOR_BUFFER_BIT);
			} else {
				//we must just be removing shaders.
				foreach(const gles2::shader_program_ptr& s, active_fb_shaders_) {
					if(std::count(shaders.begin(), shaders.end(), s) == 0) {
						apply_shader_to_frame_buffer_texture(s, false);
					}
				}
			}
		}

		active_fb_shaders_ = shaders;
	}
}

void level::flush_frame_buffer_shaders_to_screen() const
{
	for(int n = 0; n != active_fb_shaders_.size(); ++n) {
		apply_shader_to_frame_buffer_texture(active_fb_shaders_[n], n == active_fb_shaders_.size()-1);
	}
}

void level::apply_shader_to_frame_buffer_texture(gles2::shader_program_ptr shader, bool render_to_screen) const
{
	texture_frame_buffer::set_as_current_texture();

	if(render_to_screen) {
		texture_frame_buffer::set_render_to_screen();
	} else {
		texture_frame_buffer::switch_texture();
		texture_frame_buffer::set_render_to_texture();
	}

	glPushMatrix();
	glLoadIdentity();

	const GLfloat w = GLfloat(preferences::actual_screen_width());
	const GLfloat h = GLfloat(preferences::actual_screen_height());

	const GLfloat tcarray[] = { 0, 1, 0, 0, 1, 1, 1, 0 };
	const GLfloat tcarray_rotated[] = { 0, 1, 1, 1, 0, 0, 1, 0 };
	GLfloat varray[] = { 0, 0, 0, h, w, 0, w, h };

	GLfloat sprite_area[] = {0, 0, 1, 1};
	GLfloat draw_area[] = {0.0f, 0.0f, w, h};

	gles2::manager manager(shader);
	//gles2::active_shader()->shader()->set_sprite_area(sprite_area);
	gles2::active_shader()->shader()->set_draw_area(draw_area);
	gles2::active_shader()->shader()->set_cycle(cycle());
	gles2::active_shader()->shader()->vertex_array(2, GL_FLOAT, GL_FALSE, 0, varray);
	gles2::active_shader()->shader()->texture_array(2, GL_FLOAT, GL_FALSE, 0, 
		preferences::screen_rotated() ? tcarray_rotated : tcarray);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	glPopMatrix();
}

void level::shaders_updated()
{
	foreach(FrameBufferShaderEntry& e, fb_shaders_) {
		e.shader.reset();
	}
}
#endif

void level::calculate_lighting(int x, int y, int w, int h) const
{
	if(!dark_ || editor_ || texture_frame_buffer::unsupported()) {
		return;
	}

	//find all the lights in the level
	static std::vector<const light*> lights;
	lights.clear();
	foreach(const entity_ptr& c, active_chars_) {
		foreach(const light_ptr& lt, c->lights()) {
			lights.push_back(lt.get());
		}
	}

	{
		glBlendFunc(GL_ONE, GL_ONE);
		rect screen_area(x, y, w, h);
		const texture_frame_buffer::render_scope scope;

		glClearColor(dark_color_.r()/255.0, dark_color_.g()/255.0, dark_color_.b()/255.0, dark_color_.a()/255.0);
		glClear(GL_COLOR_BUFFER_BIT);
		const unsigned char color[] = { (unsigned char)dark_color_.r(), (unsigned char)dark_color_.g(), (unsigned char)dark_color_.b(), (unsigned char)dark_color_.a() };
		foreach(const light* lt, lights) {
			lt->draw(screen_area, color);
		}
	}

	//now blit the light buffer onto the screen
	texture_frame_buffer::set_as_current_texture();

	glPushMatrix();
	glLoadIdentity();

	const GLfloat tcarray[] = { 0, 0, 0, 1, 1, 0, 1, 1 };
	const GLfloat tcarray_rotated[] = { 0, 1, 1, 1, 0, 0, 1, 0 };
	GLfloat varray[] = { 0, (GLfloat)h, 0, 0, (GLfloat)w, (GLfloat)h, (GLfloat)w, 0 };
#if defined(USE_SHADERS)
	gles2::active_shader()->prepare_draw();
	gles2::active_shader()->shader()->vertex_array(2, GL_FLOAT, GL_FALSE, 0, varray);
	gles2::active_shader()->shader()->texture_array(2, GL_FLOAT, GL_FALSE, 0, 
		preferences::screen_rotated() ? tcarray_rotated : tcarray);
#else
	glVertexPointer(2, GL_FLOAT, 0, varray);
	glTexCoordPointer(2, GL_FLOAT, 0,
	               preferences::screen_rotated() ? tcarray_rotated : tcarray);
#endif
	glBlendFunc(GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glPopMatrix();
}

void level::draw_debug_solid(int x, int y, int w, int h) const
{
	if(preferences::show_debug_hitboxes() == false) {
		return;
	}

	const int tile_x = x/TileSize - 2;
	const int tile_y = y/TileSize - 2;

	for(int xpos = 0; xpos < w/TileSize + 4; ++xpos) {
		for(int ypos = 0; ypos < h/TileSize + 4; ++ypos) {
			const tile_pos pos(tile_x + xpos, tile_y + ypos);
			const tile_solid_info* info = solid_.find(pos);
			if(info == NULL) {
				continue;
			}

			const int xpixel = (tile_x + xpos)*TileSize;
			const int ypixel = (tile_y + ypos)*TileSize;

			if(info->all_solid) {
				graphics::draw_rect(rect(xpixel, ypixel, TileSize, TileSize), info->info.damage ? graphics::color(255, 0, 0, 196) : graphics::color(255, 255, 255, 196));
			} else {
				std::vector<GLshort> v;
#if !defined(USE_SHADERS)
				glDisable(GL_TEXTURE_2D);
				glDisableClientState(GL_TEXTURE_COORD_ARRAY);
#endif
				for(int suby = 0; suby != TileSize; ++suby) {
					for(int subx = 0; subx != TileSize; ++subx) {
						if(info->bitmap.test(suby*TileSize + subx)) {
							v.push_back(xpixel + subx + 1);
							v.push_back(ypixel + suby + 1);
						}
					}
				}

				if(!v.empty()) {
					if(info->info.damage) {
						glColor4ub(255, 0, 0, 196);
					} else {
						glColor4ub(255, 255, 255, 196);
					}

#if defined(USE_SHADERS)
					glPointSize(1.0f);
					gles2::manager gles2_manager(gles2::get_simple_shader());
					gles2::active_shader()->shader()->vertex_array(2, GL_SHORT, 0, 0, &v[0]);
#else
					glPointSize(1);
					glVertexPointer(2, GL_SHORT, 0, &v[0]);
#endif
					glDrawArrays(GL_POINTS, 0, v.size()/2);
				}
#if !defined(USE_SHADERS)
				glEnableClientState(GL_TEXTURE_COORD_ARRAY);
				glEnable(GL_TEXTURE_2D);
#endif
			}
		}
	}

	glColor4ub(255, 255, 255, 255);
}

void level::draw_background(int x, int y, int rotation) const
{
	if(show_background_ == false) {
		return;
	}

	if(water_) {
		water_->begin_drawing();
	}

	foreach(const entity_ptr& c, active_chars_) {
		c->setup_drawing();
	}

	if(background_) {
#ifdef USE_SHADERS
		active_fb_shaders_.clear();
		frame_buffer_enter_zorder(-1000000);
#endif
		static std::vector<rect> opaque_areas;
		opaque_areas.clear();
		int screen_width = graphics::screen_width();
		int screen_height = graphics::screen_height();
		if(last_draw_position().zoom < 1.0) {
			screen_width /= last_draw_position().zoom;
			screen_height /= last_draw_position().zoom;
		}

		rect screen_area(x, y, screen_width, screen_height);
		foreach(const rect& r, opaque_rects_) {
			if(rects_intersect(r, screen_area)) {

				rect intersection = intersection_rect(r, screen_area);

				if(intersection.w() == screen_area.w() || intersection.h() == screen_area.h()) {
					rect result[2];
					const int nrects = rect_difference(screen_area, intersection, result);
					ASSERT_LOG(nrects <= 2, "TOO MANY RESULTS " << nrects << " IN " << screen_area << " - " << intersection);
					if(nrects < 1) {
						//background is completely obscured, so return
						return;
					} else if(nrects == 1) {
						screen_area = result[0];
					} else {
						opaque_areas.push_back(intersection);
					}
				} else if(intersection.w()*intersection.h() >= TileSize*TileSize*8) {
					opaque_areas.push_back(intersection);
				}
			}
		}

		background_->draw(x, y, screen_area, opaque_areas, rotation, cycle());
	} else {
		glClearColor(0.0, 0.0, 0.0, 0.0);
		glClear(GL_COLOR_BUFFER_BIT);
	}
}

void level::process()
{
	formula_profiler::instrument instrumentation("LEVEL_PROCESS");
	if(!gui_algorithm_.empty()) {
		foreach(gui_algorithm_ptr g, gui_algorithm_) {
			g->process(*this);
		}
	}

	const int LevelPreloadFrequency = 500; //10 seconds
	//see if we have levels to pre-load. Load one periodically.
	if((cycle_%LevelPreloadFrequency) == 0) {
		const int index = cycle_/LevelPreloadFrequency;
		if(index < preloads_.size()) {
			preload_level(preloads_[index]);
		}
	}

	controls::read_local_controls();

#if !defined(__native_client__)
	multiplayer::send_and_receive();
#endif

	do_processing();

	if(speech_dialogs_.empty() == false) {
		if(speech_dialogs_.top()->process()) {
			speech_dialogs_.pop();
		}
	}

	editor_dragging_objects_ = false;

#if defined(USE_ISOMAP)
	if(iso_world_) {
		iso_world_->process();
	}
#endif
}

void level::process_draw()
{
	std::vector<entity_ptr> chars = active_chars_;
	foreach(const entity_ptr& e, chars) {
		e->handle_event(OBJECT_EVENT_DRAW);
	}
}

namespace {
bool compare_entity_num_parents(const entity_ptr& a, const entity_ptr& b) {
	bool a_human = false, b_human = false;
	const int deptha = a->parent_depth(&a_human);
	const int depthb = b->parent_depth(&b_human);
	if(a_human != b_human) {
		return b_human;
	}

	const bool standa = a->standing_on().get() ? true : false;
	const bool standb = b->standing_on().get() ? true : false;
	return deptha < depthb || deptha == depthb && standa < standb ||
	     deptha == depthb && standa == standb && a->is_human() < b->is_human();
}
}

void level::set_active_chars()
{
	const decimal inverse_zoom_level = zoom_level_ != decimal(0) ? (decimal(1.0)/zoom_level_) : decimal(0);
	const int zoom_buffer = (std::max(decimal(0.0),(inverse_zoom_level - decimal(1.0))) * graphics::screen_width()).as_int(); //pad the screen if we're zoomed out so stuff now-visible becomes active 
	const int screen_left = last_draw_position().x/100 - zoom_buffer;
	const int screen_right = last_draw_position().x/100 + graphics::screen_width() + zoom_buffer;
	const int screen_top = last_draw_position().y/100 - zoom_buffer;
	const int screen_bottom = last_draw_position().y/100 + graphics::screen_height() + zoom_buffer;

	const rect screen_area(screen_left, screen_top, screen_right - screen_left, screen_bottom - screen_top);
	active_chars_.clear();
	foreach(entity_ptr& c, chars_) {
		const bool is_active = c->is_active(screen_area) || c->use_absolute_screen_coordinates();

		if(is_active) {
			if(c->group() >= 0) {
				assert(c->group() < groups_.size());
				const entity_group& group = groups_[c->group()];
				active_chars_.insert(active_chars_.end(), group.begin(), group.end());
			} else {
				active_chars_.push_back(c);
			}
		} else { //char is inactive
			if( c->dies_on_inactive() ){
				if(c->label().empty() == false) {
					c->die_with_no_event();
					chars_by_label_.erase(c->label());
				}
				
				c = entity_ptr(); //can't delete it while iterating over the container, so we null it for later removal
			}
		}
	}

	chars_.erase(std::remove(chars_.begin(), chars_.end(), entity_ptr()), chars_.end());

	std::sort(active_chars_.begin(), active_chars_.end());
	active_chars_.erase(std::unique(active_chars_.begin(), active_chars_.end()), active_chars_.end());
	std::sort(active_chars_.begin(), active_chars_.end(), zorder_compare);
}

void level::do_processing()
{
	if(cycle_ == 0) {
		const std::vector<entity_ptr> chars = chars_;
		foreach(const entity_ptr& e, chars) {
			e->handle_event(OBJECT_EVENT_START_LEVEL);
			e->create_object();
		}
	}

	if(!paused_) {
		++cycle_;
	}

	if(!player_) {
		return;
	}

	const int ticks = SDL_GetTicks();
	set_active_chars();
	detect_user_collisions(*this);

	
/*
	std::cerr << "SUMMARY " << cycle_ << ": ";
	foreach(const entity_ptr& e, chars_) {
		std::cerr << e->debug_description() << "(" << (e->is_human() ? "HUMAN," : "") << e->centi_x() << "," << e->centi_y() << "):";
	}

	std::cerr << "\n";
	*/

	int checksum = 0;
	foreach(const entity_ptr& e, chars_) {
		checksum += e->x() + e->y();
	}

	controls::set_checksum(cycle_, checksum);

	const int ActivationDistance = 700;

	std::vector<entity_ptr> active_chars = active_chars_;
	std::sort(active_chars.begin(), active_chars.end(), compare_entity_num_parents);
	if(time_freeze_ >= 1000) {
		time_freeze_ -= 1000;
		active_chars = chars_immune_from_time_freeze_;
	}

	while(!active_chars.empty()) {
		new_chars_.clear();
		foreach(const entity_ptr& c, active_chars) {
			if(!c->destroyed() && (chars_by_label_.count(c->label()) || c->is_human())) {
				c->process(*this);
			}
	
			if(c->destroyed() && !c->is_human()) {
				if(player_ && !c->respawn() && c->get_id() != -1) {
					player_->is_human()->object_destroyed(id(), c->get_id());
				}
	
				erase_char(c);
			}
		}

		active_chars = new_chars_;
		active_chars_.insert(active_chars_.end(), new_chars_.begin(), new_chars_.end());
	}

	if(water_) {
		water_->process(*this);
	}

	solid_chars_.clear();
}

void level::erase_char(entity_ptr c)
{

	if(c->label().empty() == false) {
		chars_by_label_.erase(c->label());
	}
	chars_.erase(std::remove(chars_.begin(), chars_.end(), c), chars_.end());
	if(c->group() >= 0) {
		assert(c->group() < groups_.size());
		entity_group& group = groups_[c->group()];
		group.erase(std::remove(group.begin(), group.end(), c), group.end());
	}

	solid_chars_.clear();
}

bool level::is_solid(const level_solid_map& map, const entity& e, const std::vector<point>& points, const surface_info** surf_info) const
{
	const tile_solid_info* info = NULL;
	int prev_x = INT_MIN, prev_y = INT_MIN;

	const frame& current_frame = e.current_frame();
	
	for(std::vector<point>::const_iterator p = points.begin(); p != points.end(); ++p) {
		int x, y;
		if(prev_x != INT_MIN) {
			const int diff_x = (p->x - (p-1)->x) * (e.face_right() ? 1 : -1);
			const int diff_y = p->y - (p-1)->y;

			x = prev_x + diff_x;
			y = prev_y + diff_y;
			
			if(x < 0 || y < 0 || x >= TileSize || y >= TileSize) {
				//we need to recalculate the info, since we've stepped into
				//another tile.
				prev_x = INT_MIN;
			}
		}
		
		if(prev_x == INT_MIN) {
			x = e.x() + (e.face_right() ? p->x : (current_frame.width() - 1 - p->x));
			y = e.y() + p->y;

			tile_pos pos(x/TileSize, y/TileSize);
			x = x%TileSize;
			y = y%TileSize;
			if(x < 0) {
				pos.first--;
				x += TileSize;
			}

			if(y < 0) {
				pos.second--;
				y += TileSize;
			}

			info = map.find(pos);
		}

		if(info != NULL) {
			if(info->all_solid) {
				if(surf_info) {
					*surf_info = &info->info;
				}

				return true;
			}
		
			const int index = y*TileSize + x;
			if(info->bitmap.test(index)) {
				if(surf_info) {
					*surf_info = &info->info;
				}

				return true;
			}
		}

		prev_x = x;
		prev_y = y;
	}

	return false;
}

bool level::is_solid(const level_solid_map& map, int x, int y, const surface_info** surf_info) const
{
	tile_pos pos(x/TileSize, y/TileSize);
	x = x%TileSize;
	y = y%TileSize;
	if(x < 0) {
		pos.first--;
		x += TileSize;
	}

	if(y < 0) {
		pos.second--;
		y += TileSize;
	}

	const tile_solid_info* info = map.find(pos);
	if(info != NULL) {
		if(info->all_solid) {
			if(surf_info) {
				*surf_info = &info->info;
			}

			return true;
		}
		
		const int index = y*TileSize + x;
		if(info->bitmap.test(index)) {
			if(surf_info) {
				*surf_info = &info->info;
			}

			return true;
		} else {
			return false;
		}
	}

	return false;
}

bool level::standable(const rect& r, const surface_info** info) const
{
	const int ybegin = r.y();
	const int yend = r.y2();
	const int xbegin = r.x();
	const int xend = r.x2();

	for(int y = ybegin; y != yend; ++y) {
		for(int x = xbegin; x != xend; ++x) {
			if(standable(x, y, info)) {
				return true;
			}
		}
	}

	return false;
}

bool level::standable(int x, int y, const surface_info** info) const
{
	if(is_solid(solid_, x, y, info) || is_solid(standable_, x, y, info)) {
	   return true;
	}

	return false;
}

bool level::standable_tile(int x, int y, const surface_info** info) const
{
	if(is_solid(solid_, x, y, info) || is_solid(standable_, x, y, info)) {
		return true;
	}
	
	return false;
}


bool level::solid(int x, int y, const surface_info** info) const
{
	return is_solid(solid_, x, y, info);
}

bool level::solid(const entity& e, const std::vector<point>& points, const surface_info** info) const
{
	return is_solid(solid_, e, points, info);
}

bool level::solid(int xbegin, int ybegin, int w, int h, const surface_info** info) const
{
	const int xend = xbegin + w;
	const int yend = ybegin + h;

	for(int y = ybegin; y != yend; ++y) {
		for(int x = xbegin; x != xend; ++x) {
			if(solid(x, y, info)) {
				return true;
			}
		}
	}

	return false;
}

bool level::solid(const rect& r, const surface_info** info) const
{
	//TODO: consider optimizing this function.
	const int ybegin = r.y();
	const int yend = r.y2();
	const int xbegin = r.x();
	const int xend = r.x2();

	for(int y = ybegin; y != yend; ++y) {
		for(int x = xbegin; x != xend; ++x) {
			if(solid(x, y, info)) {
				return true;
			}
		}
	}

	return false;
}

bool level::may_be_solid_in_rect(const rect& r) const
{
	int x = r.x();
	int y = r.y();
	tile_pos pos(x/TileSize, y/TileSize);
	x = x%TileSize;
	y = y%TileSize;
	if(x < 0) {
		pos.first--;
		x += TileSize;
	}

	if(y < 0) {
		pos.second--;
		y += TileSize;
	}

	const int x2 = (x + r.w())/TileSize + ((x + r.w())%TileSize ? 1 : 0);
	const int y2 = (y + r.h())/TileSize + ((y + r.h())%TileSize ? 1 : 0);

	for(int ypos = 0; ypos < y2; ++ypos) {
		for(int xpos = 0; xpos < x2; ++xpos) {
			if(solid_.find(tile_pos(pos.first + xpos, pos.second + ypos))) {
				return true;
			}
		}
	}

	return false;
}

void level::set_solid_area(const rect& r, bool solid)
{
	std::string empty_info;
	for(int y = r.y(); y < r.y2(); ++y) {
		for(int x = r.x(); x < r.x2(); ++x) {
			set_solid(solid_, x, y, 100, 100, 0, empty_info, solid);
		}
	}
}

entity_ptr level::board(int x, int y) const
{
	for(std::vector<entity_ptr>::const_iterator i = active_chars_.begin();
	    i != active_chars_.end(); ++i) {
		const entity_ptr& c = *i;
		if(c->boardable_vehicle() && c->point_collides(x, y)) {
			return c;
		}
	}

	return entity_ptr();
}

void level::add_tile(const level_tile& t)
{
	std::vector<level_tile>::iterator itor = std::lower_bound(tiles_.begin(), tiles_.end(), t, level_tile_zorder_comparer());
	tiles_.insert(itor, t);
	add_tile_solid(t);
	layers_.insert(t.zorder);
	prepare_tiles_for_drawing();
}

bool level::add_tile_rect(int zorder, int x1, int y1, int x2, int y2, const std::string& str)
{
	return add_tile_rect_vector(zorder, x1, y1, x2, y2, std::vector<std::string>(1, str));
}

bool level::add_tile_rect_vector(int zorder, int x1, int y1, int x2, int y2, const std::vector<std::string>& tiles)
{
	if(x1 > x2) {
		std::swap(x1, x2);
	}

	if(y1 > y2) {
		std::swap(y1, y2);
	}
	return add_tile_rect_vector_internal(zorder, x1, y1, x2, y2, tiles);
}

void level::add_hex_tile_rect(int zorder, int x1, int y1, int x2, int y2, const std::string& tile)
{
	add_hex_tile_rect_vector(zorder, x1, y1, x2, y2, std::vector<std::string>(1, tile));
}

void level::add_hex_tile_rect_vector(int zorder, int x1, int y1, int x2, int y2, const std::vector<std::string>& tiles)
{
	if(x1 > x2) {
		std::swap(x1, x2);
	}

	if(y1 > y2) {
		std::swap(y1, y2);
	}
	add_hex_tile_rect_vector_internal(zorder, x1, y1, x2, y2, tiles);
}

void level::set_tile_layer_speed(int zorder, int x_speed, int y_speed)
{
	tile_map& m = tile_maps_[zorder];
	m.set_zorder(zorder);
	m.set_speed(x_speed, y_speed);
}

void level::refresh_tile_rect(int x1, int y1, int x2, int y2)
{
	rebuild_tiles_rect(rect(x1-128, y1-128, (x2 - x1) + 256, (y2 - y1) + 256));
}

namespace {
int round_tile_size(int n)
{
	if(n >= 0) {
		return n - n%TileSize;
	} else {
		n = -n + TileSize;
		return -(n - n%TileSize);
	}
}

}

bool level::add_tile_rect_vector_internal(int zorder, int x1, int y1, int x2, int y2, const std::vector<std::string>& tiles)
{
	if(tiles.empty()) {
		return false;
	}

	if(x1 > x2) {
		std::swap(x1, x2);
	}

	if(y1 > y2) {
		std::swap(y1, y2);
	}

	x1 = round_tile_size(x1);
	y1 = round_tile_size(y1);
	x2 = round_tile_size(x2 + TileSize);
	y2 = round_tile_size(y2 + TileSize);

	tile_map& m = tile_maps_[zorder];
	m.set_zorder(zorder);

	bool changed = false;

	int index = 0;
	for(int x = x1; x < x2; x += TileSize) {
		for(int y = y1; y < y2; y += TileSize) {
			changed = m.set_tile(x, y, tiles[index]) || changed;
			if(index+1 < tiles.size()) {
				++index;
			}
		}
	}

	return changed;
}

bool level::add_hex_tile_rect_vector_internal(int zorder, int x1, int y1, int x2, int y2, const std::vector<std::string>& tiles)
{
	if(tiles.empty()) {
		return false;
	}

	if(x1 > x2) {
		std::swap(x1, x2);
	}

	if(y1 > y2) {
		std::swap(y1, y2);
	}

	std::map<int, hex::hex_map_ptr>::iterator it = hex_maps_.find(zorder);
	if(it == hex_maps_.end()) {
		hex_maps_[zorder] = hex::hex_map_ptr(new hex::hex_map());
	}
	hex::hex_map_ptr& m = hex_maps_[zorder];
	m->set_zorder(zorder);

	bool changed = false;
	int index = 0;
	const int HexTileSize = 72;
	for(int x = x1; x <= x2; x += HexTileSize) {
		for(int y = y1; y <= y2; y += HexTileSize) {
			const point p = hex::hex_map::get_tile_pos_from_pixel_pos(x, y);
			changed = m->set_tile(p.x, p.y, tiles[index]) || changed;
			if(index+1 < tiles.size()) {
				++index;
			}
		}
	}

	return changed;
}

void level::get_tile_rect(int zorder, int x1, int y1, int x2, int y2, std::vector<std::string>& tiles) const
{
	if(x1 > x2) {
		std::swap(x1, x2);
	}

	if(y1 > y2) {
		std::swap(y1, y2);
	}

	x1 = round_tile_size(x1);
	y1 = round_tile_size(y1);
	x2 = round_tile_size(x2 + TileSize);
	y2 = round_tile_size(y2 + TileSize);

	std::map<int, tile_map>::const_iterator map_iterator = tile_maps_.find(zorder);
	if(map_iterator == tile_maps_.end()) {
		tiles.push_back("");
		return;
	}
	const tile_map& m = map_iterator->second;

	for(int x = x1; x < x2; x += TileSize) {
		for(int y = y1; y < y2; y += TileSize) {
			tiles.push_back(m.get_tile_from_pixel_pos(x, y));
		}
	}
}

void level::get_all_tiles_rect(int x1, int y1, int x2, int y2, std::map<int, std::vector<std::string> >& tiles) const
{
	for(std::set<int>::const_iterator i = layers_.begin(); i != layers_.end(); ++i) {
		if(hidden_layers_.count(*i)) {
			continue;
		}

		std::vector<std::string> cleared;
		get_tile_rect(*i, x1, y1, x2, y2, cleared);
		if(std::count(cleared.begin(), cleared.end(), "") != cleared.size()) {
			tiles[*i].swap(cleared);
		}
	}
}

void level::get_all_hex_tiles_rect(int x1, int y1, int x2, int y2, std::map<int, std::vector<std::string> >& tiles) const
{
	for(std::set<int>::const_iterator i = layers_.begin(); i != layers_.end(); ++i) {
		if(hidden_layers_.count(*i)) {
			continue;
		}

		std::vector<std::string> cleared;
		get_hex_tile_rect(*i, x1, y1, x2, y2, cleared);
		if(std::count(cleared.begin(), cleared.end(), "") != cleared.size()) {
			tiles[*i].swap(cleared);
		}
	}
}

void level::get_hex_tile_rect(int zorder, int x1, int y1, int x2, int y2, std::vector<std::string>& tiles) const
{
	if(x1 > x2) {
		std::swap(x1, x2);
	}

	if(y1 > y2) {
		std::swap(y1, y2);
	}

	std::map<int, hex::hex_map_ptr>::const_iterator map_iterator = hex_maps_.find(zorder);
	if(map_iterator == hex_maps_.end()) {
		tiles.push_back("");
		return;
	}
	const hex::hex_map_ptr& m = map_iterator->second;

	const int HexTileSize = 72;
	for(int x = x1; x < x2; x += HexTileSize) {
		for(int y = y1; y < y2; y += HexTileSize) {
			hex::hex_object_ptr p = m->get_tile_from_pixel_pos(x, y);
			tiles.push_back((p) ? p->type() : "");
		}
	}
}

bool level::clear_tile_rect(int x1, int y1, int x2, int y2)
{
	if(x1 > x2) {
		std::swap(x1, x2);
	}

	if(y1 > y2) {
		std::swap(y1, y2);
	}

	bool changed = false;
	std::vector<std::string> v(1, "");
	for(std::set<int>::const_iterator i = layers_.begin(); i != layers_.end(); ++i) {
		if(hidden_layers_.count(*i)) {
			continue;
		}

		if(add_tile_rect_vector_internal(*i, x1, y1, x2, y2, v)) {
			changed = true;
		}
	}
	
	return changed;
}

void level::clear_hex_tile_rect(int x1, int y1, int x2, int y2)
{
	if(x1 > x2) {
		std::swap(x1, x2);
	}

	if(y1 > y2) {
		std::swap(y1, y2);
	}

	bool changed = false;
	std::vector<std::string> v(1, "");
	for(std::set<int>::const_iterator i = layers_.begin(); i != layers_.end(); ++i) {
		if(hidden_layers_.count(*i)) {
			continue;
		}

		if(add_hex_tile_rect_vector_internal(*i, x1, y1, x2, y2, v)) {
			changed = true;
		}
	}
}

void level::add_tile_solid(const level_tile& t)
{
	//zorders greater than 1000 are considered in the foreground and so
	//have no solids.
	if(t.zorder >= 1000) {
		return;
	}

	if(t.object->width() > widest_tile_) {
		widest_tile_ = t.object->width();
	}

	if(t.object->height() > highest_tile_) {
		highest_tile_ = t.object->height();
	}

	const const_level_object_ptr& obj = t.object;
	if(obj->all_solid()) {
		add_solid_rect(t.x, t.y, t.x + obj->width(), t.y + obj->height(), obj->friction(), obj->traction(), obj->damage(), obj->info());
		return;
	}

	if(obj->has_solid()) {
		for(int y = 0; y != obj->height(); ++y) {
			for(int x = 0; x != obj->width(); ++x) {
				int xpos = x;
				if(!t.face_right) {
					xpos = obj->width() - x - 1;
				}
				if(obj->is_solid(xpos, y)) {
					if(obj->is_passthrough()) {
						add_standable(t.x + x, t.y + y, obj->friction(), obj->traction(), obj->damage(), obj->info());
					} else {
						add_solid(t.x + x, t.y + y, obj->friction(), obj->traction(), obj->damage(), obj->info());
					}
				}
			}
		}
	}
}

struct tile_on_point {
	int x_, y_;
	tile_on_point(int x, int y) : x_(x), y_(y)
	{}

	bool operator()(const level_tile& t) const {
		return x_ >= t.x && y_ >= t.y && x_ < t.x + t.object->width() && y_ < t.y + t.object->height();
	}
};

bool level::remove_tiles_at(int x, int y)
{
	const int nitems = tiles_.size();
	tiles_.erase(std::remove_if(tiles_.begin(), tiles_.end(), tile_on_point(x,y)), tiles_.end());
	const bool result = nitems != tiles_.size();
	prepare_tiles_for_drawing();
	return result;
}

std::vector<point> level::get_solid_contiguous_region(int xpos, int ypos) const
{
	std::vector<point> result;

	xpos = round_tile_size(xpos);
	ypos = round_tile_size(ypos);

	tile_pos base(xpos/TileSize, ypos/TileSize);
	const tile_solid_info* info = solid_.find(base);
	if(info == NULL || info->all_solid == false && info->bitmap.any() == false) {
		return result;
	}

	std::set<tile_pos> positions;
	positions.insert(base);

	int last_count = -1;
	while(positions.size() != last_count) {
		last_count = positions.size();

		std::vector<tile_pos> new_positions;
		foreach(const tile_pos& pos, positions) {
			new_positions.push_back(std::make_pair(pos.first-1, pos.second));
			new_positions.push_back(std::make_pair(pos.first+1, pos.second));
			new_positions.push_back(std::make_pair(pos.first, pos.second-1));
			new_positions.push_back(std::make_pair(pos.first, pos.second+1));
		}

		foreach(const tile_pos& pos, new_positions) {
			if(positions.count(pos)) {
				continue;
			}

			const tile_solid_info* info = solid_.find(pos);
			if(info == NULL || info->all_solid == false && info->bitmap.any() == false) {
				continue;
			}

			positions.insert(pos);
		}
	}

	foreach(const tile_pos& pos, positions) {
		result.push_back(point(pos.first, pos.second));
	}

	return result;
}

const level_tile* level::get_tile_at(int x, int y) const
{
	std::vector<level_tile>::const_iterator i = std::find_if(tiles_.begin(), tiles_.end(), tile_on_point(x,y));
	if(i != tiles_.end()) {
		return &*i;
	} else {
		return NULL;
	}
}

void level::remove_character(entity_ptr e)
{
	e->being_removed();
	if(e->label().empty() == false) {
		chars_by_label_.erase(e->label());
	}
	chars_.erase(std::remove(chars_.begin(), chars_.end(), e), chars_.end());
	solid_chars_.erase(std::remove(solid_chars_.begin(), solid_chars_.end(), e), solid_chars_.end());
	active_chars_.erase(std::remove(active_chars_.begin(), active_chars_.end(), e), active_chars_.end());
}

std::vector<entity_ptr> level::get_characters_in_rect(const rect& r, int screen_xpos, int screen_ypos) const
{
	std::vector<entity_ptr> res;
	foreach(entity_ptr c, chars_) {
		if(object_classification_hidden(*c)) {
			continue;
		}
		custom_object* obj = dynamic_cast<custom_object*>(c.get());

		const int xP = c->midpoint().x + ((c->parallax_scale_millis_x() - 1000)*screen_xpos)/1000 
			+ (obj->use_absolute_screen_coordinates() ? screen_xpos : 0);
		const int yP = c->midpoint().y + ((c->parallax_scale_millis_y() - 1000)*screen_ypos)/1000 
			+ (obj->use_absolute_screen_coordinates() ? screen_ypos : 0);
		if(point_in_rect(point(xP, yP), r)) {
			res.push_back(c);
		}
	}

	return res;
}

std::vector<entity_ptr> level::get_characters_at_point(int x, int y, int screen_xpos, int screen_ypos) const
{
	std::vector<entity_ptr> result;
	foreach(entity_ptr c, chars_) {
		if(object_classification_hidden(*c) || c->truez()) {
			continue;
		}

		const int xP = x + ((1000 - (c->parallax_scale_millis_x()))* screen_xpos )/1000
			- (c->use_absolute_screen_coordinates() ? screen_xpos : 0);
		const int yP = y + ((1000 - (c->parallax_scale_millis_y()))* screen_ypos )/1000
			- (c->use_absolute_screen_coordinates() ? screen_ypos : 0);

		if(!c->is_alpha(xP, yP)) {
			result.push_back(c);
		}
	}
	
	return result;
}

namespace {
bool compare_entities_by_spawned(entity_ptr a, entity_ptr b)
{
	return a->spawned_by().size() < b->spawned_by().size();
}
}

entity_ptr level::get_next_character_at_point(int x, int y, int screen_xpos, int screen_ypos) const
{
	std::vector<entity_ptr> v = get_characters_at_point(x, y, screen_xpos, screen_ypos);
	if(v.empty()) {
		return entity_ptr();
	}

	std::sort(v.begin(), v.end(), compare_entities_by_spawned);

	if(editor_selection_.empty()) {
		return v.front();
	}

	std::vector<entity_ptr>::iterator itor = std::find(v.begin(), v.end(), editor_selection_.back());
	if(itor == v.end()) {
		return v.front();
	}

	++itor;
	if(itor == v.end()) {
		itor = v.begin();
	}

	return *itor;
}

void level::add_solid_rect(int x1, int y1, int x2, int y2, int friction, int traction, int damage, const std::string& info_str)
{
	if((x1%TileSize) != 0 || (y1%TileSize) != 0 ||
	   (x2%TileSize) != 0 || (y2%TileSize) != 0) {
		for(int y = y1; y < y2; ++y) {
			for(int x = x1; x < x2; ++x) {
				add_solid(x, y, friction, traction, damage, info_str);
			}
		}

		return;
	}

	for(int y = y1; y < y2; y += TileSize) {
		for(int x = x1; x < x2; x += TileSize) {
			tile_pos pos(x/TileSize, y/TileSize);
			tile_solid_info& s = solid_.insert_or_find(pos);
			s.all_solid = true;
			s.info.friction = friction;
			s.info.traction = traction;

			if(s.info.damage >= 0) {
				s.info.damage = std::min(s.info.damage, damage);
			} else {
				s.info.damage = damage;
			}

			if(info_str.empty() == false) {
				s.info.info = surface_info::get_info_str(info_str);
			}
		}
	}
}

void level::add_solid(int x, int y, int friction, int traction, int damage, const std::string& info)
{
	set_solid(solid_, x, y, friction, traction, damage, info);
}

void level::add_standable(int x, int y, int friction, int traction, int damage, const std::string& info)
{
	set_solid(standable_, x, y, friction, traction, damage, info);
}

void level::set_solid(level_solid_map& map, int x, int y, int friction, int traction, int damage, const std::string& info_str, bool solid)
{
	tile_pos pos(x/TileSize, y/TileSize);
	x = x%TileSize;
	y = y%TileSize;
	if(x < 0) {
		pos.first--;
		x += TileSize;
	}

	if(y < 0) {
		pos.second--;
		y += TileSize;
	}
	const int index = y*TileSize + x;
	tile_solid_info& info = map.insert_or_find(pos);

	if(info.info.damage >= 0) {
		info.info.damage = std::min(info.info.damage, damage);
	} else {
		info.info.damage = damage;
	}

	if(solid) {
		info.info.friction = friction;
		info.info.traction = traction;
		info.bitmap.set(index);
	} else {
		if(info.all_solid) {
			info.all_solid = false;
			info.bitmap.set();
		}

		info.bitmap.reset(index);
	}

	if(info_str.empty() == false) {
		info.info.info = surface_info::get_info_str(info_str);
	}
}

void level::add_multi_player(entity_ptr p)
{
	last_touched_player_ = p;
	p->get_player_info()->set_player_slot(players_.size());
	ASSERT_LOG(!g_player_type || g_player_type->match(variant(p.get())), "Player object being added to level does not match required player type. " << p->debug_description() << " is not a " << g_player_type->to_string());
	players_.push_back(p);
	chars_.push_back(p);
	if(p->label().empty() == false) {
		chars_by_label_[p->label()] = p;
	}
	layers_.insert(p->zorder());
}

void level::add_player(entity_ptr p)
{
	chars_.erase(std::remove(chars_.begin(), chars_.end(), player_), chars_.end());
	last_touched_player_ = player_ = p;
	ASSERT_LOG(!g_player_type || g_player_type->match(variant(p.get())), "Player object being added to level does not match required player type. " << p->debug_description() << " is not a " << g_player_type->to_string());
	if(players_.empty()) {
		player_->get_player_info()->set_player_slot(players_.size());
		players_.push_back(player_);
	} else {
		ASSERT_LOG(player_->is_human(), "level::add_player(): Tried to add player to the level that isn't human.");
		player_->get_player_info()->set_player_slot(0);
		players_[0] = player_;
	}

	p->add_to_level();

	assert(player_);
	chars_.push_back(p);

	//remove objects that have already been destroyed
	const std::vector<int>& destroyed_objects = player_->get_player_info()->get_objects_destroyed(id());
	for(int n = 0; n != chars_.size(); ++n) {
		if(chars_[n]->respawn() == false && std::binary_search(destroyed_objects.begin(), destroyed_objects.end(), chars_[n]->get_id())) {
			if(chars_[n]->label().empty() == false) {
				chars_by_label_.erase(chars_[n]->label());
			}
			chars_[n] = entity_ptr();
		}
	}

	if(!editor_) {
		const int difficulty = current_difficulty();
		for(int n = 0; n != chars_.size(); ++n) {
			if(chars_[n].get() != NULL && !chars_[n]->appears_at_difficulty(difficulty)) {
				chars_[n] = entity_ptr();
			}
		}
	}

	chars_.erase(std::remove(chars_.begin(), chars_.end(), entity_ptr()), chars_.end());
}

void level::add_character(entity_ptr p)
{
	if(solid_chars_.empty() == false && p->solid()) {
		solid_chars_.push_back(p);
	}

	ASSERT_LOG(p->label().empty() == false, "Entity has no label");

	if(p->label().empty() == false) {
		entity_ptr& target = chars_by_label_[p->label()];
		if(!target) {
			target = p;
		} else {
			while(chars_by_label_[p->label()]) {
				p->set_label(formatter() << p->label() << rand());
			}

			chars_by_label_[p->label()] = p;
		}
	}

	if(p->is_human()) {
		add_player(p);
	} else {
		chars_.push_back(p);
	}

	p->add_to_level();

	layers_.insert(p->zorder());

	const int screen_left = last_draw_position().x/100;
	const int screen_right = last_draw_position().x/100 + graphics::screen_width();
	const int screen_top = last_draw_position().y/100;
	const int screen_bottom = last_draw_position().y/100 + graphics::screen_height();

	const rect screen_area(screen_left, screen_top, screen_right - screen_left, screen_bottom - screen_top);
	if(!active_chars_.empty() && (p->is_active(screen_area) || p->use_absolute_screen_coordinates())) {
		new_chars_.push_back(p);
	}
	p->being_added();
}

void level::add_draw_character(entity_ptr p)
{
	active_chars_.push_back(p);
}

void level::force_enter_portal(const portal& p)
{
	entered_portal_active_ = true;
	entered_portal_ = p;
}

const level::portal* level::get_portal() const
{
	if(entered_portal_active_) {
		entered_portal_active_ = false;
		return &entered_portal_;
	}

	if(!player_) {
		return NULL;
	}

	const rect& r = player_->body_rect();
	if(r.x() < boundaries().x() && left_portal_.level_dest.empty() == false) {
		return &left_portal_;
	}

	if(r.x2() > boundaries().x2() && right_portal_.level_dest.empty() == false) {
		return &right_portal_;
	}
	foreach(const portal& p, portals_) {
		if(rects_intersect(r, p.area) && (p.automatic || player_->enter())) {
			return &p;
		}
	}

	return NULL;
}

int level::group_size(int group) const
{
	int res = 0;
	foreach(const entity_ptr& c, active_chars_) {
		if(c->group() == group) {
			++res;
		}
	}

	return res;
}

void level::set_character_group(entity_ptr c, int group_num)
{
	assert(group_num < static_cast<int>(groups_.size()));

	//remove any current grouping
	if(c->group() >= 0) {
		assert(c->group() < groups_.size());
		entity_group& group = groups_[c->group()];
		group.erase(std::remove(group.begin(), group.end(), c), group.end());
	}

	c->set_group(group_num);

	if(group_num >= 0) {
		entity_group& group = groups_[group_num];
		group.push_back(c);
	}
}

int level::add_group()
{
	groups_.resize(groups_.size() + 1);
	return groups_.size() - 1;
}

void level::editor_select_object(entity_ptr c)
{
	if(!c) {
		return;
	}
	editor_selection_.push_back(c);
}

void level::editor_deselect_object(entity_ptr c)
{
	editor_selection_.erase(std::remove(editor_selection_.begin(), editor_selection_.end(), c), editor_selection_.end());
}

void level::editor_clear_selection()
{
	editor_selection_.clear();
}

const std::string& level::get_background_id() const
{
	if(background_) {
		return background_->id();
	} else {
		static const std::string empty_string;
		return empty_string;
	}
}

void level::set_background_by_id(const std::string& id)
{
	background_ = background::get(id, background_palette_);
}

BEGIN_DEFINE_CALLABLE_NOBASE(level)
DEFINE_FIELD(cycle, "int")
	return variant(obj.cycle_);
DEFINE_SET_FIELD
	obj.cycle_ = value.as_int();
DEFINE_FIELD(player, "custom_obj")
	ASSERT_LOG(obj.last_touched_player_, "No player found in level");
	return variant(obj.last_touched_player_.get());
DEFINE_FIELD(player_info, "object")
	ASSERT_LOG(obj.last_touched_player_, "No player found in level");
	return variant(obj.last_touched_player_.get());
DEFINE_FIELD(in_dialog, "bool")
	//boost::intrusive_ptr<const game_logic::formula_callable_definition> def(variant_type::get_builtin("level")->get_definition());
	return variant::from_bool(obj.in_dialog_);
DEFINE_FIELD(local_player, "null|custom_obj")
	ASSERT_LOG(obj.player_, "No player found in level");
	return variant(obj.player_.get());
DEFINE_FIELD(num_active, "int")
	return variant(static_cast<int>(obj.active_chars_.size()));
DEFINE_FIELD(active_chars, "[custom_obj]")
	std::vector<variant> v;
	foreach(const entity_ptr& e, obj.active_chars_) {
		v.push_back(variant(e.get()));
	}
	return variant(&v);
DEFINE_FIELD(chars, "[custom_obj]")
	std::vector<variant> v;
	foreach(const entity_ptr& e, obj.chars_) {
		v.push_back(variant(e.get()));
	}
	return variant(&v);
DEFINE_FIELD(players, "[custom_obj]")
	std::vector<variant> v;
	foreach(const entity_ptr& e, obj.players()) {
		v.push_back(variant(e.get()));
	}
	return variant(&v);
DEFINE_FIELD(in_editor, "bool")
	return variant(obj.editor_);
DEFINE_FIELD(zoom, "decimal")
	return variant(obj.zoom_level_);
DEFINE_SET_FIELD
	obj.zoom_level_ = value.as_decimal();
DEFINE_FIELD(focus, "[custom_obj]")
	std::vector<variant> v;
	foreach(const entity_ptr& e, obj.focus_override_) {
		v.push_back(variant(e.get()));
	}
	return variant(&v);
DEFINE_SET_FIELD
	obj.focus_override_.clear();
	for(int n = 0; n != value.num_elements(); ++n) {
		entity* e = value[n].try_convert<entity>();
		if(e) {
			obj.focus_override_.push_back(entity_ptr(e));
		}
	}

DEFINE_FIELD(gui, "[object]")
	std::vector<variant> v;
	if(!obj.gui_algorithm_.empty()) {
		foreach(gui_algorithm_ptr g, obj.gui_algorithm_) {
			v.push_back(variant(g->get_object()));
		}
	}
	return variant(&v);

DEFINE_FIELD(id, "string")
	return variant(obj.id_);

DEFINE_FIELD(dimensions, "[int]")
	std::vector<variant> v;
	v.push_back(variant(obj.boundaries_.x()));
	v.push_back(variant(obj.boundaries_.y()));
	v.push_back(variant(obj.boundaries_.x2()));
	v.push_back(variant(obj.boundaries_.y2()));
	return variant(&v);
DEFINE_SET_FIELD
	ASSERT_EQ(value.num_elements(), 4);
	obj.boundaries_ = rect(value[0].as_int(), value[1].as_int(), value[2].as_int() - value[0].as_int(), value[3].as_int() - value[1].as_int());

DEFINE_FIELD(music_volume, "decimal")
	return variant(sound::get_engine_music_volume());
DEFINE_SET_FIELD
	sound::set_engine_music_volume(value.as_decimal().as_float());
DEFINE_FIELD(paused, "bool")
	return variant::from_bool(obj.paused_);
DEFINE_SET_FIELD
	const bool new_value = value.as_bool();
	if(new_value != obj.paused_) {
		obj.paused_ = new_value;
		if(obj.paused_) {
			obj.before_pause_controls_backup_.reset(new controls::control_backup_scope);
		} else {
			if(&obj != current_ptr()) {
				obj.before_pause_controls_backup_->cancel();
			}
			obj.before_pause_controls_backup_.reset();
		}
		foreach(entity_ptr e, obj.chars_) {
			e->mutate_value("paused", value);
		}
	}

DEFINE_FIELD(module_args, "object")
	return variant(module::get_module_args().get());

#if defined(USE_BOX2D)
DEFINE_FIELD(world, "object")
	return variant(box2d::world::our_world_ptr().get());
#else
DEFINE_FIELD(world, "null")
	return variant();
#endif

DEFINE_FIELD(time_freeze, "int")
	return variant(obj.time_freeze_);
DEFINE_SET_FIELD
	obj.time_freeze_ = value.as_int();
DEFINE_FIELD(chars_immune_from_time_freeze, "[custom_obj]")
	std::vector<variant> v;
	foreach(const entity_ptr& e, obj.chars_immune_from_time_freeze_) {
		v.push_back(variant(e.get()));
	}
	return variant(&v);
DEFINE_SET_FIELD
	obj.chars_immune_from_time_freeze_.clear();
	for(int n = 0; n != value.num_elements(); ++n) {
		entity_ptr e(value[n].try_convert<entity>());
		if(e) {
			obj.chars_immune_from_time_freeze_.push_back(e);
		}
	}

DEFINE_FIELD(segment_width, "int")
	return variant(obj.segment_width_);
DEFINE_FIELD(segment_height, "int")
	return variant(obj.segment_height_);
DEFINE_FIELD(num_segments, "int")
	return variant(unsigned(obj.sub_levels_.size()));

DEFINE_FIELD(camera_position, "[int, int, int, int]")
	std::vector<variant> pos;
	pos.reserve(4);
	pos.push_back(variant(last_draw_position().x/100));
	pos.push_back(variant(last_draw_position().y/100));
	pos.push_back(variant(graphics::screen_width()));
	pos.push_back(variant(graphics::screen_height()));
	return variant(&pos);
DEFINE_SET_FIELD

	ASSERT_EQ(value.num_elements(), 2);
	last_draw_position().x_pos = last_draw_position().x = value[0].as_int();
	last_draw_position().y_pos = last_draw_position().y = value[1].as_int();

DEFINE_FIELD(camera_target, "[int,int]")
	std::vector<variant> pos;
	pos.reserve(2);

	pos.push_back(variant(last_draw_position().target_xpos));
	pos.push_back(variant(last_draw_position().target_ypos));

	return variant(&pos);
	

DEFINE_FIELD(debug_properties, "[string]")
	return vector_to_variant(obj.debug_properties_);
DEFINE_SET_FIELD
	if(value.is_null()) {
		obj.debug_properties_.clear();
	} else if(value.is_string()) {
		obj.debug_properties_.clear();
		obj.debug_properties_.push_back(value.as_string());
	} else {
		obj.debug_properties_ = value.as_list_string();
	}

DEFINE_FIELD(hexmap, "null|object")
	if(obj.hex_maps_.empty() == false) {
		return variant(obj.hex_maps_.rbegin()->second.get());
	} else {
		return variant();
	}

DEFINE_FIELD(hexmaps, "{int -> object}")
	std::map<variant, variant> m;
	std::map<int, hex::hex_map_ptr>::const_iterator it = obj.hex_maps_.begin();
	while(it != obj.hex_maps_.end()) {
		m[variant(it->first)] = variant(it->second.get());
		++it;
	}
	return variant(&m);

DEFINE_FIELD(shader, "null|object")
#if defined(USE_SHADERS)
	return variant(obj.shader_.get());
#else
	return variant();
#endif

DEFINE_FIELD(is_paused, "bool")
	if(level_runner::get_current()) {
		return variant::from_bool(level_runner::get_current()->is_paused());
	}

	return variant(false);

DEFINE_FIELD(editor_selection, "[custom_obj]")
	std::vector<variant> result;
	foreach(entity_ptr s, obj.editor_selection_) {
		result.push_back(variant(s.get()));
	}

	return variant(&result);

#if defined(USE_SHADERS)
DEFINE_FIELD(frame_buffer_shaders, "[{begin_zorder: int, end_zorder: int, shader: object|null, shader_info: map|string}]")
	std::vector<variant> v;
	foreach(const FrameBufferShaderEntry& e, obj.fb_shaders_) {
		std::map<variant,variant> m;
		m[variant("begin_zorder")] = variant(e.begin_zorder);
		m[variant("end_zorder")] = variant(e.end_zorder);
		m[variant("shader_info")] = e.shader_node;

		m[variant("shader")] = variant(e.shader.get());
		v.push_back(variant(&m));
	}

	obj.fb_shaders_variant_ = variant(&v);
	return obj.fb_shaders_variant_;

DEFINE_SET_FIELD

	obj.fb_shaders_variant_ = variant();
	obj.fb_shaders_.clear();
	foreach(const variant& v, value.as_list()) {
		FrameBufferShaderEntry e;
		e.begin_zorder = v["begin_zorder"].as_int();
		e.end_zorder = v["end_zorder"].as_int();
		e.shader_node = v["shader_info"];

		if(v.has_key("shader")) {
			e.shader.reset(v["shader"].try_convert<gles2::shader_program>());
		}

		if(!e.shader) {
			if(e.shader_node.is_string()) {
				e.shader = gles2::shader_program::get_global(e.shader_node.as_string());
			} else {
				e.shader.reset(new gles2::shader_program(e.shader_node));
			}
		}

		obj.fb_shaders_.push_back(e);
	}
#endif

DEFINE_FIELD(preferences, "object")
	return variant(preferences::get_settings_obj());
DEFINE_FIELD(lock_screen, "null|[int]")
	if(obj.lock_screen_.get()) {
		std::vector<variant> v;
		v.push_back(variant(obj.lock_screen_->x));
		v.push_back(variant(obj.lock_screen_->y));
		return variant(&v);
	} else {
		return variant();
	}
DEFINE_SET_FIELD
	if(value.is_list()) {
		obj.lock_screen_.reset(new point(value[0].as_int(), value[1].as_int()));
	} else {
		obj.lock_screen_.reset();
	}

#if defined(USE_ISOMAP)
DEFINE_FIELD(isoworld, "builtin world")
	ASSERT_LOG(obj.iso_world_, "No world present in level");
	return variant(obj.iso_world_.get());
DEFINE_SET_FIELD_TYPE("builtin world|map|null")
	if(value.is_null()) {
		obj.iso_world_.reset(); 
	} else {
		obj.iso_world_.reset(new voxel::world(value));
	}

DEFINE_FIELD(camera, "builtin camera_callable")
	return variant(obj.camera_.get());
DEFINE_SET_FIELD
	if(value.is_null()) {
		obj.camera_.reset(new camera_callable()); 
	} else {
		obj.camera_.reset(new camera_callable(value));
	}
#endif

DEFINE_FIELD(mouselook, "bool")
#if defined(USE_ISOMAP)
	return variant::from_bool(obj.is_mouselook_enabled());
#else
	return variant::from_bool(false);
#endif
DEFINE_SET_FIELD
#if defined(USE_ISOMAP)
	obj.set_mouselook(value.as_bool());
#endif

DEFINE_FIELD(mouselook_invert, "bool")
#if defined(USE_ISOMAP)
	return variant::from_bool(obj.is_mouselook_inverted());
#else
	return variant::from_bool(false);
#endif
DEFINE_SET_FIELD
#if defined(USE_ISOMAP)
	obj.set_mouselook_inverted(value.as_bool());
#endif

DEFINE_FIELD(suspended_level, "builtin level")
	ASSERT_LOG(obj.suspended_level_, "Query of suspended_level when there is no suspended level");
	return variant(obj.suspended_level_.get());

END_DEFINE_CALLABLE(level)

int level::camera_rotation() const
{
	if(!camera_rotation_) {
		return 0;
	}

	return camera_rotation_->execute(*this).as_int();
}

bool level::is_underwater(const rect& r, rect* res_water_area, variant* v) const
{
	return water_ && water_->is_underwater(r, res_water_area, v);
}

void level::get_current(const entity& e, int* velocity_x, int* velocity_y) const
{
	if(e.mass() == 0) {
		return;
	}

	int delta_x = 0, delta_y = 0;
	if(is_underwater(e.body_rect())) {
		delta_x += *velocity_x;
		delta_y += *velocity_y;
		water_->get_current(e, &delta_x, &delta_y);
		delta_x -= *velocity_x;
		delta_y -= *velocity_y;
	}

	delta_x /= e.mass();
	delta_y /= e.mass();

	foreach(const entity_ptr& c, active_chars_) {
		if(c.get() != &e) {
			delta_x += *velocity_x;
			delta_y += *velocity_y;
			c->generate_current(e, &delta_x, &delta_y);
			delta_x -= *velocity_x;
			delta_y -= *velocity_y;
		}
	}

	*velocity_x += delta_x;
	*velocity_y += delta_y;
}

water& level::get_or_create_water()
{
	if(!water_) {
		water_.reset(new water);
	}

	return *water_;
}

entity_ptr level::get_entity_by_label(const std::string& label)
{
	std::map<std::string, entity_ptr>::iterator itor = chars_by_label_.find(label);
	if(itor != chars_by_label_.end()) {
		return itor->second;
	}

	return entity_ptr();
}

const_entity_ptr level::get_entity_by_label(const std::string& label) const
{
	std::map<std::string, entity_ptr>::const_iterator itor = chars_by_label_.find(label);
	if(itor != chars_by_label_.end()) {
		return itor->second;
	}

	return const_entity_ptr();
}

void level::get_all_labels(std::vector<std::string>& labels) const
{
	for(std::map<std::string, entity_ptr>::const_iterator i = chars_by_label_.begin(); i != chars_by_label_.end(); ++i) {
		labels.push_back(i->first);
	}
}

const std::vector<entity_ptr>& level::get_solid_chars() const
{
	if(solid_chars_.empty()) {
		foreach(const entity_ptr& e, chars_) {
			if(e->solid() || e->platform()) {
				solid_chars_.push_back(e);
			}
		}
	}

	return solid_chars_;
}

void level::begin_movement_script(const std::string& key, entity& e)
{
	std::map<std::string, movement_script>::const_iterator itor = movement_scripts_.find(key);
	if(itor == movement_scripts_.end()) {
		return;
	}

	active_movement_scripts_.push_back(itor->second.begin_execution(e));
}

void level::end_movement_script()
{
	if(!active_movement_scripts_.empty()) {
		active_movement_scripts_.pop_back();
	}
}

bool level::can_interact(const rect& body) const
{
	foreach(const portal& p, portals_) {
		if(p.automatic == false && rects_intersect(body, p.area)) {
			return true;
		}
	}

	foreach(const entity_ptr& c, active_chars_) {
		if(c->can_interact_with() && rects_intersect(body, c->body_rect()) &&
		   intersection_rect(body, c->body_rect()).w() >= std::min(body.w(), c->body_rect().w())/2) {
			return true;
		}
	}

	return false;
}

void level::replay_from_cycle(int ncycle)
{
	const int cycles_ago = cycle_ - ncycle;
	if(cycles_ago <= 0) {
		return;
	}

	int index = static_cast<int>(backups_.size()) - cycles_ago;
	ASSERT_GE(index, 0);

	const int cycle_to_play_until = cycle_;
	restore_from_backup(*backups_[index]);
	ASSERT_EQ(cycle_, ncycle);
	backups_.erase(backups_.begin() + index, backups_.end());
	while(cycle_ < cycle_to_play_until) {
		backup();
		do_processing();
	}
}

void level::backup()
{
	if(backups_.empty() == false && backups_.back()->cycle == cycle_) {
		return;
	}

	std::map<entity_ptr, entity_ptr> entity_map;

	backup_snapshot_ptr snapshot(new backup_snapshot);
	snapshot->rng_seed = rng::get_seed();
	snapshot->cycle = cycle_;
	snapshot->chars.reserve(chars_.size());


	foreach(const entity_ptr& e, chars_) {
		snapshot->chars.push_back(e->backup());
		entity_map[e] = snapshot->chars.back();

		if(snapshot->chars.back()->is_human()) {
			snapshot->players.push_back(snapshot->chars.back());
			if(e == player_) {
				snapshot->player = snapshot->players.back();
			}
		}
	}

	foreach(entity_group& g, groups_) {
		snapshot->groups.push_back(entity_group());

		foreach(entity_ptr e, g) {
			std::map<entity_ptr, entity_ptr>::iterator i = entity_map.find(e);
			if(i != entity_map.end()) {
				snapshot->groups.back().push_back(i->second);
			}
		}
	}

	foreach(const entity_ptr& e, snapshot->chars) {
		e->map_entities(entity_map);
	}

	snapshot->last_touched_player = last_touched_player_;

	backups_.push_back(snapshot);
	if(backups_.size() > 250) {

		for(std::deque<backup_snapshot_ptr>::iterator i = backups_.begin();
		    i != backups_.begin() + 1; ++i) {
			foreach(const entity_ptr& e, (*i)->chars) {
				//kill off any references this entity holds, to workaround
				//circular references causing things to stick around.
				e->cleanup_references();
			}
		}
		backups_.erase(backups_.begin(), backups_.begin() + 1);
	}
}

int level::earliest_backup_cycle() const
{
	if(backups_.empty()) {
		return cycle_;
	} else {
		return backups_.front()->cycle;
	}
}

void level::reverse_one_cycle()
{
	if(backups_.empty()) {
		return;
	}

	restore_from_backup(*backups_.back());
	backups_.pop_back();
}

void level::reverse_to_cycle(int ncycle)
{
	if(backups_.empty()) {
		return;
	}

	std::cerr << "REVERSING FROM " << cycle_ << " TO " << ncycle << "...\n";

	while(backups_.size() > 1 && backups_.back()->cycle > ncycle) {
		std::cerr << "REVERSING PAST " << backups_.back()->cycle << "...\n";
		backups_.pop_back();
	}

	std::cerr << "GOT TO CYCLE: " << backups_.back()->cycle << "\n";

	reverse_one_cycle();
}

void level::restore_from_backup(backup_snapshot& snapshot)
{
	rng::set_seed(snapshot.rng_seed);
	cycle_ = snapshot.cycle;
	chars_ = snapshot.chars;
	players_ = snapshot.players;
	player_ = snapshot.player;
	groups_ = snapshot.groups;
	last_touched_player_ = snapshot.last_touched_player;
	active_chars_.clear();

	solid_chars_.clear();

	chars_by_label_.clear();
	foreach(const entity_ptr& e, chars_) {
		if(e->label().empty() == false) {
			chars_by_label_[e->label()] = e;
		}
	}

	for(const entity_ptr& ch : snapshot.chars) {
		ch->handle_event(OBJECT_EVENT_LOAD);
	}
}

std::vector<entity_ptr> level::trace_past(entity_ptr e, int ncycle)
{
	backup();
	int prev_cycle = -1;
	std::vector<entity_ptr> result;
	std::deque<backup_snapshot_ptr>::reverse_iterator i = backups_.rbegin();
	while(i != backups_.rend() && (*i)->cycle >= ncycle) {
		const backup_snapshot& snapshot = **i;
		if(prev_cycle != -1 && snapshot.cycle == prev_cycle) {
			++i;
			continue;
		}

		prev_cycle = snapshot.cycle;

		foreach(const entity_ptr& ghost, snapshot.chars) {
			if(ghost->label() == e->label()) {
				result.push_back(ghost);
				break;
			}
		}
		++i;
	}

	return result;
}

std::vector<entity_ptr> level::predict_future(entity_ptr e, int ncycles)
{
	disable_flashes_scope flashes_disabled_scope;
	const controls::control_backup_scope ctrl_backup_scope;

	backup();
	backup_snapshot_ptr snapshot = backups_.back();
	backups_.pop_back();

	const size_t starting_backups = backups_.size();

	int begin_time = SDL_GetTicks();
	int nframes = 0;

	const int controls_end = controls::local_controls_end();
	std::cerr << "PREDICT FUTURE: " << cycle_ << "/" << controls_end << "\n";
	while(cycle_ < controls_end) {
		try {
			const assert_recover_scope safe_scope;
			process();
			backup();
			++nframes;
		} catch(validation_failure_exception&) {
			std::cerr << "ERROR WHILE PREDICTING FUTURE...\n";
			break;
		}
	}

	std::cerr << "TOOK " << (SDL_GetTicks() - begin_time) << "ms TO MOVE FORWARD " << nframes << " frames\n";

	begin_time = SDL_GetTicks();

	std::vector<entity_ptr> result = trace_past(e, -1);

	std::cerr << "TOOK " << (SDL_GetTicks() - begin_time) << "ms to TRACE PAST OF " << result.size() << " FRAMES\n";

	backups_.resize(starting_backups);
	restore_from_backup(*snapshot);

	return result;
}

void level::transfer_state_to(level& lvl)
{
	backup();
	lvl.restore_from_backup(*backups_.back());
	backups_.pop_back();
}

void level::get_tile_layers(std::set<int>* all_layers, std::set<int>* hidden_layers)
{
	if(all_layers) {
		foreach(const level_tile& t, tiles_) {
			all_layers->insert(t.zorder);
		}
	}

	if(hidden_layers) {
		*hidden_layers = hidden_layers_;
	}
}

void level::hide_tile_layer(int layer, bool is_hidden)
{
	if(is_hidden) {
		hidden_layers_.insert(layer);
	} else {
		hidden_layers_.erase(layer);
	}
}

void level::hide_object_classification(const std::string& classification, bool hidden)
{
	if(hidden) {
		hidden_classifications_.insert(classification);
	} else {
		hidden_classifications_.erase(classification);
	}
}

bool level::object_classification_hidden(const entity& e) const
{
#ifndef NO_EDITOR
	return e.editor_info() && hidden_object_classifications().count(e.editor_info()->classification());
#else
	return false;
#endif
}

void level::editor_freeze_tile_updates(bool value)
{
	if(value) {
		++editor_tile_updates_frozen_;
	} else {
		--editor_tile_updates_frozen_;
		if(editor_tile_updates_frozen_ == 0) {
			rebuild_tiles();
		}
	}
}

decimal level::zoom_level() const
{
	return zoom_level_;
}

void level::add_speech_dialog(boost::shared_ptr<speech_dialog> d)
{
	speech_dialogs_.push(d);
}

void level::remove_speech_dialog()
{
	if(speech_dialogs_.empty() == false) {
		speech_dialogs_.pop();
	}
}

boost::shared_ptr<const speech_dialog> level::current_speech_dialog() const
{
	if(speech_dialogs_.empty()) {
		return boost::shared_ptr<const speech_dialog>();
	}

	return speech_dialogs_.top();
}

bool entity_in_current_level(const entity* e)
{
	const level& lvl = level::current();
	return std::find(lvl.get_chars().begin(), lvl.get_chars().end(), e) != lvl.get_chars().end();
}

void level::add_sub_level(const std::string& lvl, int xoffset, int yoffset, bool add_objects)
{

	const std::map<std::string, sub_level_data>::iterator itor = sub_levels_.find(lvl);
	ASSERT_LOG(itor != sub_levels_.end(), "SUB LEVEL NOT FOUND: " << lvl);

	if(itor->second.active && add_objects) {
		remove_sub_level(lvl);
	}

	const int xdiff = xoffset - itor->second.xoffset;
	const int ydiff = yoffset - itor->second.yoffset;

	itor->second.xoffset = xoffset - itor->second.xbase;
	itor->second.yoffset = yoffset - itor->second.ybase;

	std::cerr << "ADDING SUB LEVEL: " << lvl << "(" << itor->second.lvl->boundaries() << ") " << itor->second.xbase << ", " << itor->second.ybase << " -> " << itor->second.xoffset << ", " << itor->second.yoffset << "\n";

	itor->second.active = true;
	level& sub = *itor->second.lvl;

	if(add_objects) {
		const int difficulty = current_difficulty();
		foreach(entity_ptr e, sub.chars_) {
			if(e->is_human()) {
				continue;
			}
	
			entity_ptr c = e->clone();
			if(!c) {
				continue;
			}

			relocate_object(c, c->x() + itor->second.xoffset, c->y() + itor->second.yoffset);
			if(c->appears_at_difficulty(difficulty)) {
				add_character(c);
				c->handle_event(OBJECT_EVENT_START_LEVEL);

				itor->second.objects.push_back(c);
			}
		}
	}

	foreach(solid_color_rect& r, sub.solid_color_rects_) {
		r.area = rect(r.area.x() + xdiff, r.area.y() + ydiff, r.area.w(), r.area.h());
	}

	build_solid_data_from_sub_levels();
}

void level::remove_sub_level(const std::string& lvl)
{
	const std::map<std::string, sub_level_data>::iterator itor = sub_levels_.find(lvl);
	ASSERT_LOG(itor != sub_levels_.end(), "SUB LEVEL NOT FOUND: " << lvl);

	if(itor->second.active) {
		foreach(entity_ptr& e, itor->second.objects) {
			if(std::find(active_chars_.begin(), active_chars_.end(), e) == active_chars_.end()) {
				remove_character(e);
			}
		}

		itor->second.objects.clear();
	}

	itor->second.active = false;
}

void level::build_solid_data_from_sub_levels()
{
	solid_ = solid_base_;
	standable_ = standable_base_;
	solid_.clear();
	standable_.clear();

	for(std::map<std::string, sub_level_data>::const_iterator i = sub_levels_.begin(); i != sub_levels_.end(); ++i) {
		if(!i->second.active) {
			continue;
		}

		const int xoffset = i->second.xoffset/TileSize;
		const int yoffset = i->second.yoffset/TileSize;
		solid_.merge(i->second.lvl->solid_, xoffset, yoffset);
		standable_.merge(i->second.lvl->standable_, xoffset, yoffset);
	}
}

void level::adjust_level_offset(int xoffset, int yoffset)
{
	game_logic::map_formula_callable* callable(new game_logic::map_formula_callable);
	variant holder(callable);
	callable->add("xshift", variant(xoffset));
	callable->add("yshift", variant(yoffset));
	foreach(entity_ptr e, chars_) {
		e->shift_position(xoffset, yoffset);
		e->handle_event(OBJECT_EVENT_COSMIC_SHIFT, callable);
	}

	boundaries_ = rect(boundaries_.x() + xoffset, boundaries_.y() + yoffset, boundaries_.w(), boundaries_.h());

	for(std::map<std::string, sub_level_data>::iterator i = sub_levels_.begin();
	    i != sub_levels_.end(); ++i) {
		if(i->second.active) {
			add_sub_level(i->first, i->second.xoffset + xoffset + i->second.xbase, i->second.yoffset + yoffset + i->second.ybase, false);
		}
	}

	last_draw_position().x += xoffset*100;
	last_draw_position().y += yoffset*100;
	last_draw_position().focus_x += xoffset;
	last_draw_position().focus_y += yoffset;
}

bool level::relocate_object(entity_ptr e, int new_x, int new_y)
{
	const int orig_x = e->x();
	const int orig_y = e->y();

	const int delta_x = new_x - orig_x;
	const int delta_y = new_y - orig_y;

	e->set_pos(new_x, new_y);

	if(!place_entity_in_level(*this, *e)) {
		//if we can't place the object due to solidity, then cancel
		//the movement.
		e->set_pos(orig_x, orig_y);
		return false;
	}


#ifndef NO_EDITOR
	//update any x/y co-ordinates to be the same relative to the object's
	//new position.
	if(e->editor_info()) {
		foreach(const editor_variable_info& var, e->editor_info()->vars_and_properties()) {
			const variant value = e->query_value(var.variable_name());
			switch(var.type()) {
			case editor_variable_info::XPOSITION:
				if(value.is_int()) {
					e->handle_event("editor_changing_variable");
					e->mutate_value(var.variable_name(), variant(value.as_int() + delta_x));
					e->handle_event("editor_changed_variable");
				}
				break;
			case editor_variable_info::YPOSITION:
				if(value.is_int()) {
					e->handle_event("editor_changing_variable");
					e->mutate_value(var.variable_name(), variant(value.as_int() + delta_y));
					e->handle_event("editor_changed_variable");
				}
				break;
			case editor_variable_info::TYPE_POINTS:
				if(value.is_list()) {
					std::vector<variant> new_value;
					foreach(variant point, value.as_list()) {
						std::vector<variant> p = point.as_list();
						if(p.size() == 2) {
							p[0] = variant(p[0].as_int() + delta_x);
							p[1] = variant(p[1].as_int() + delta_y);
							new_value.push_back(variant(&p));
						}
					}
					e->handle_event("editor_changing_variable");
					e->mutate_value(var.variable_name(), variant(&new_value));
					e->handle_event("editor_changed_variable");
				}
			default:
				break;
			}
		}
	}
#endif // !NO_EDITOR

	return true;
}

void level::record_zorders()
{
	foreach(const level_tile& t, tiles_) {
		t.object->record_zorder(t.zorder);
	}
}

#if defined(USE_ISOMAP)
const float* level::projection() const
{
	ASSERT_LOG(camera_ != NULL, "level::projection(): Accessing camera_ but is null");
	return camera_->projection();
}

const float* level::view() const
{
	ASSERT_LOG(camera_ != NULL, "level::view(): Accessing camera_ but is null");
	return camera_->view();
}

std::vector<entity_ptr> level::get_characters_at_world_point(const glm::vec3& pt)
{
	const double tolerance = 0.25;
	std::vector<entity_ptr> result;
	foreach(entity_ptr c, chars_) {
		if(object_classification_hidden(*c) || c->truez() == false) {
			continue;
		}

		if(abs(pt.x - c->tx()) < tolerance 
			&& abs(pt.y - c->ty()) < tolerance 
			&& abs(pt.z - c->tz()) < tolerance) {
			result.push_back(c);
		}
	}
	return result;	
}
#endif

int level::current_difficulty() const
{
	if(!editor_ && preferences::force_difficulty() != INT_MIN) {
		return preferences::force_difficulty();
	}

	if(!last_touched_player_) {
		return 0;
	}

	playable_custom_object* p = dynamic_cast<playable_custom_object*>(last_touched_player_.get());
	if(!p) {
		return 0;
	}

	return p->difficulty();
}

bool level::gui_event(const SDL_Event &event)
{
	foreach(gui_algorithm_ptr g, gui_algorithm_) {
		if(g->gui_event(*this, event)) {
			return true;
		}
	}
	return false;
}

void level::launch_new_module(const std::string& module_id, game_logic::const_formula_callable_ptr callable)
{
	module::reload(module_id);
	reload_level_paths();
	custom_object_type::reload_file_paths();
	font::reload_font_paths();
#if defined(USE_SHADERS)
	gles2::init_default_shader();
#endif


	const std::vector<entity_ptr> players = this->players();
	foreach(entity_ptr e, players) {
		this->remove_character(e);
	}

	if(callable) {
		module::set_module_args(callable);
	}

	level::portal p;
	p.level_dest = "titlescreen.cfg";
	p.dest_starting_pos = true;
	p.automatic = true;
	p.transition = "instant";
	p.saved_game = true; //makes it use the player in there.
	force_enter_portal(p);
}

std::pair<std::vector<level_tile>::const_iterator, std::vector<level_tile>::const_iterator> level::tiles_at_loc(int x, int y) const
{
	x = round_tile_size(x);
	y = round_tile_size(y);

	if(tiles_by_position_.size() != tiles_.size()) {
		tiles_by_position_ = tiles_;
		std::sort(tiles_by_position_.begin(), tiles_by_position_.end(), level_tile_pos_comparer());
	}

	std::pair<int, int> loc(x, y);
	return std::equal_range(tiles_by_position_.begin(), tiles_by_position_.end(), loc, level_tile_pos_comparer());
}

game_logic::formula_ptr level::create_formula(const variant& v)
{
	// XXX Add symbol table here?
	return game_logic::formula_ptr(new game_logic::formula(v));
}

bool level::execute_command(const variant& var)
{
	bool result = true;
	if(var.is_null()) {
		return result;
	}

	if(var.is_list()) {
		const int num_elements = var.num_elements();
		for(int n = 0; n != num_elements; ++n) {
			if(var[n].is_null() == false) {
				result = execute_command(var[n]) && result;
			}
		}
	} else {
		game_logic::command_callable* cmd = var.try_convert<game_logic::command_callable>();
		if(cmd != NULL) {
			cmd->run_command(*this);
		}
	}
	return result;
}

UTILITY(correct_solidity)
{
	std::vector<std::string> files;
	sys::get_files_in_dir(preferences::level_path(), &files);
	foreach(const std::string& file, files) {
		if(file.size() <= 4 || std::string(file.end()-4, file.end()) != ".cfg") {
			continue;
		}

		boost::intrusive_ptr<level> lvl(new level(file));
		lvl->finish_loading();
		lvl->set_as_current_level();

		foreach(entity_ptr c, lvl->get_chars()) {
			if(entity_collides_with_level(*lvl, *c, MOVE_NONE)) {
				if(place_entity_in_level_with_large_displacement(*lvl, *c)) {
					std::cerr << "LEVEL: " << lvl->id() << " CORRECTED " << c->debug_description() << "\n";
				} else {
					std::cerr << "LEVEL: " << lvl->id() << " FAILED TO CORRECT " << c->debug_description() << "\n";
				}
			}

			c->handle_event("editor_removed");
			c->handle_event("editor_added");
		}

		sys::write_file(preferences::level_path() + file, lvl->write().write_json(true));
	}
}

UTILITY(compile_levels)
{
#ifndef IMPLEMENT_SAVE_PNG
	std::cerr
		<< "This build wasn't done with IMPLEMENT_SAVE_PNG defined. "
		<< "Consquently image files will not be written, aborting requested operation."
		<< std::endl;
	return;
#endif

	preferences::compiling_tiles = true;

	std::cerr << "COMPILING LEVELS...\n";

	std::map<std::string, std::string> file_paths;
	module::get_unique_filenames_under_dir(preferences::level_path(), &file_paths);

	variant_builder index_node;

	for(std::map<std::string, std::string>::const_iterator i = file_paths.begin(); i != file_paths.end(); ++i) {
		if(strstr(i->second.c_str(), "/Unused")) {
			continue;
		}

		const std::string& file = module::get_id(i->first);
		std::cerr << "LOADING LEVEL '" << file << "'\n";
		boost::intrusive_ptr<level> lvl(new level(file));
		lvl->finish_loading();
		lvl->record_zorders();
		module::write_file("data/compiled/level/" + file, lvl->write().write_json(true));
		std::cerr << "SAVING LEVEL TO MODULE: data/compiled/level/" + file + "\n";

		variant_builder level_summary;
		level_summary.add("level", lvl->id());
		level_summary.add("title", lvl->title());
		level_summary.add("music", lvl->music());
		index_node.add("level", level_summary.build());
	}

	module::write_file("data/compiled/level_index.cfg", index_node.build().write_json(true));

	level_object::write_compiled();
}

BENCHMARK(level_solid)
{
	//benchmark which tells us how long level::solid takes.
	static level* lvl = new level("stairway-to-heaven.cfg");
	BENCHMARK_LOOP {
		lvl->solid(rng::generate()%1000, rng::generate()%1000);
	}
}

BENCHMARK(load_nene)
{
	BENCHMARK_LOOP {
		level lvl("to-nenes-house.cfg");
	}
}

BENCHMARK(load_all_levels)
{
	std::vector<std::string> files;
	module::get_files_in_dir(preferences::level_path(), &files);
	BENCHMARK_LOOP {
		foreach(const std::string& file, files) {
			boost::intrusive_ptr<level> lvl(new level(file));
		}
	}
}

UTILITY(load_and_save_all_levels)
{
	std::map<std::string, std::string> files;
	module::get_unique_filenames_under_dir(preferences::level_path(), &files);
	for(std::map<std::string,std::string>::const_iterator i = files.begin();
	    i != files.end(); ++i) {
		const std::string& file = i->first;
		std::cerr << "LOAD_LEVEL '" << file << "'\n";
		boost::intrusive_ptr<level> lvl(new level(file));
		lvl->finish_loading();

		const std::string path = get_level_path(file);

		std::cerr << "WRITE_LEVEL: '" << file << "' TO " << path << "\n";
		sys::write_file(path, lvl->write().write_json(true));
	}
}

