#include "autobot.hpp"
#include "pc.hpp"
#include "mob.hpp"
#include "skill.hpp"
#include "battle.hpp"
#include "path.hpp"
#include "clif.hpp"
#include "atcommand.hpp"
#include "itemdb.hpp"
#include "log.hpp"
#include <cstring>
#include <cstdlib>
#include <cstdio>

// Forward declaration
static int autobot_search_mob_sub(struct block_list* bl, va_list ap);
static bool autobot_teleport(map_session_data* sd);

// ==========================================
// Init & Cleanup
// ==========================================
void autobot_init(map_session_data* sd) {
	if (!sd) return;
	sd->bot = autobot_data(); // Reset ค่าทั้งหมดเป็น default
}

void autobot_cleanup(map_session_data* sd) {
	if (!sd) return;
	if (sd->bot.timer_id) {
		delete_timer(sd->bot.timer_id, autobot_timer);
		sd->bot.timer_id = 0;
	}
	sd->bot.enabled = false;
	sd->state.autoloot = 0; // คืนค่า autoloot
}

// ==========================================
// Teleport Logic (ปรับปรุง)
// ==========================================
static bool autobot_teleport(map_session_data* sd) {
	if (!sd->bot.auto_teleport) return false;

	bool can_teleport = false;

	// Teleport by Skill (ถ้าเปิดใช้)
	if (sd->bot.tp_by_skill && pc_checkskill(sd, 26) > 0) {
		can_teleport = true;
	}
	// Teleport by Item (ถ้าเปิดใช้)
	else if (sd->bot.tp_by_item) {
		if (pc_search_inventory(sd, 14645) >= 0 || pc_search_inventory(sd, 12887) >= 0) {
			can_teleport = true;
		}
		else {
			int idx = pc_search_inventory(sd, 601);
			if (idx >= 0) {
				pc_delitem(sd, idx, 1, 0, 1, LOG_TYPE_CONSUME);
				can_teleport = true;
			}
			else {
				idx = pc_search_inventory(sd, 23288);
				if (idx >= 0) {
					pc_delitem(sd, idx, 1, 0, 1, LOG_TYPE_CONSUME);
					can_teleport = true;
				}
			}
		}
	}

	if (can_teleport) {
		pc_randomwarp(sd, CLR_TELEPORT);
		sd->bot.target_id = 0;
		return true;
	}
	return false;
}

// ==========================================
// Mob Search Logic (ปรับปรุง)
// ==========================================
static int autobot_search_mob_sub(struct block_list* bl, va_list ap) {
	map_session_data* sd = va_arg(ap, map_session_data*);
	int* min_dist = va_arg(ap, int*);
	int* target_id = va_arg(ap, int*);

	if (!bl || bl->type != BL_MOB || status_isdead(*bl)) return 0;

	// Escape BOSS (ถ้าเปิดใช้)
	if (sd->bot.escape_boss && (status_get_mode(bl) & MD_MVP)) return 0;

	// Ignoring monster (ถ้ามี Mob ID ที่ต้องการเพิกเฉย)
	if (sd->bot.ignore_mob_id > 0) {
		mob_data* md = (mob_data*)bl;
		if (md->mob_id == sd->bot.ignore_mob_id) return 0;
	}

	if (!path_search_long(nullptr, sd->m, sd->x, sd->y, bl->x, bl->y, CELL_CHKNOPASS)) return 0;

	int dist_x = abs(sd->x - bl->x);
	int dist_y = abs(sd->y - bl->y);
	int dist = (dist_x > dist_y) ? dist_x : dist_y;

	if (dist < *min_dist) {
		*min_dist = dist;
		*target_id = bl->id;
	}
	return 0;
}

// ==========================================
// Main Timer (ทำงานทุก 500ms)
// ==========================================
TIMER_FUNC(autobot_timer) {
	map_session_data* sd = map_id2sd(id);
	if (!sd || !sd->bot.enabled) {
		delete_timer(tid, autobot_timer);
		if (sd) sd->bot.timer_id = 0;
		return 0;
	}

	// 1. Check Dead
	if (pc_isdead(sd)) {
		for (int i = 0; i < 5; i++) {
			sd->bot.buff_item_last_tick[i] = 0;
			sd->bot.buff_last_tick[i] = 0;
		}
		if (sd->bot.dead_tick == 0) {
			sd->bot.dead_tick = tick;
		}
		else if (tick_diff(tick, sd->bot.dead_tick) > 10000) {
			autobot_cleanup(sd);
			pc_setpos(sd, mapindex_name2id(sd->status.save_point.map), sd->status.save_point.x, sd->status.save_point.y, CLR_OUTSIGHT);
		}
		return 0;
	}
	else {
		sd->bot.dead_tick = 0;
	}

	// Safety checks
	if (map_getmapflag(sd->m, MF_PVP) || map_getmapflag(sd->m, MF_GVG) || map_getmapflag(sd->m, MF_BATTLEGROUND)) {
		autobot_cleanup(sd);
		return 0;
	}

	if (sd->weight * 100 / sd->max_weight >= 90) {
		autobot_cleanup(sd);
		pc_setpos(sd, mapindex_name2id(sd->status.save_point.map), sd->status.save_point.x, sd->status.save_point.y, CLR_TELEPORT);
		return 0;
	}

	if (sd->bot.flee_mvp) {
		sd->bot.flee_mvp = false;
		autobot_teleport(sd);
		return 0;
	}

	// 2. Potion
	if (sd->bot.auto_potion && tick_diff(tick, sd->bot.potion_delay_tick) >= 500) {
		bool used_pot = false;
		if (sd->bot.hp_item > 0 && sd->bot.hp_pct > 0 && ((sd->battle_status.hp * 100 / sd->battle_status.max_hp) <= sd->bot.hp_pct)) {
			int idx = pc_search_inventory(sd, sd->bot.hp_item);
			if (idx >= 0) { pc_useitem(sd, idx); used_pot = true; }
		}
		if (sd->bot.sp_item > 0 && sd->bot.sp_pct > 0 && ((sd->battle_status.sp * 100 / sd->battle_status.max_sp) <= sd->bot.sp_pct)) {
			int idx = pc_search_inventory(sd, sd->bot.sp_item);
			if (idx >= 0) { pc_useitem(sd, idx); used_pot = true; }
		}
		if (used_pot) sd->bot.potion_delay_tick = tick;
	}

	// 3. Item Buff
	if (sd->bot.auto_itembuff) {
		for (int i = 0; i < 5; i++) {
			if (sd->bot.buff_item_id[i] > 0) {
				if (sd->bot.buff_item_last_tick[i] == 0 || tick_diff(tick, sd->bot.buff_item_last_tick[i]) > sd->bot.buff_item_delay[i]) {
					int idx = pc_search_inventory(sd, sd->bot.buff_item_id[i]);
					if (idx >= 0) {
						pc_useitem(sd, idx);
						sd->bot.buff_item_last_tick[i] = tick;
						sd->bot.last_action_tick = tick;
						return 0;
					}
				}
			}
		}
	}

	// 4. Stuck Detection
	if (sd->x != sd->bot.last_x || sd->y != sd->bot.last_y) {
		sd->bot.last_x = sd->x;
		sd->bot.last_y = sd->y;
		sd->bot.last_move_tick = tick;
	}
	else if (tick_diff(tick, sd->bot.last_move_tick) > 10000 && sd->bot.target_id > 0) {
		sd->bot.last_move_tick = tick;
		autobot_teleport(sd);
		return 0;
	}

	if (tick_diff(tick, sd->bot.last_action_tick) > 30000) {
		sd->bot.last_action_tick = tick;
		autobot_teleport(sd);
		return 0;
	}

	// 5. Skill Buff (ใช้ buff_cd ที่ผู้เล่นกำหนด)
	if (sd->bot.auto_buff) {
		for (int i = 0; i < 5; i++) {
			if (sd->bot.buff_id[i] > 0 && pc_checkskill(sd, sd->bot.buff_id[i]) > 0) {
				if (sd->bot.buff_last_tick[i] == 0 || tick_diff(tick, sd->bot.buff_last_tick[i]) >= sd->bot.buff_cd[i]) {
					unit_skilluse_id(sd, sd->id, sd->bot.buff_id[i], sd->bot.buff_lv[i]);
					sd->bot.buff_last_tick[i] = tick;
					sd->bot.last_action_tick = tick;
					return 0;
				}
			}
		}
	}

	// 6. Targeting
	struct block_list* target = map_id2bl(sd->bot.target_id);
	int current_dist = 99;

	if (target) {
		int target_dist_x = abs(sd->x - target->x);
		int target_dist_y = abs(sd->y - target->y);
		current_dist = (target_dist_x > target_dist_y) ? target_dist_x : target_dist_y;
	}

	if (!target || target->type != BL_MOB || status_isdead(*target) || current_dist > 12) {
		int min_dist = 13;
		int target_id = 0;
		map_foreachinarea(autobot_search_mob_sub, sd->m, sd->x - 12, sd->y - 12, sd->x + 12, sd->y + 12, BL_MOB, sd, &min_dist, &target_id);

		if (target_id > 0) {
			sd->bot.target_id = target_id;
			target = map_id2bl(target_id);
			current_dist = min_dist;
			sd->bot.last_target_tick = tick;
			sd->bot.last_move_tick = tick;
		}
		else {
			target = nullptr;
			sd->bot.target_id = 0;
		}
	}

	if (!target) {
		if (sd->bot.auto_teleport && sd->bot.teleport_timer >= 2000) {
			if (tick_diff(tick, sd->bot.last_target_tick) > sd->bot.teleport_timer) {
				sd->bot.last_target_tick = tick;
				autobot_teleport(sd);
				return 0;
			}
		}

		if (tick_diff(tick, sd->bot.idle_walk_tick) > 500) {
			sd->bot.idle_walk_tick = tick;
			int dist_walk = 2 + (rnd() % 4);
			int dx = (rnd() % (dist_walk * 2 + 1)) - dist_walk;
			int dy = (rnd() % (dist_walk * 2 + 1)) - dist_walk;
			if (abs(dx) < 2 && abs(dy) < 2) dx = (dx >= 0) ? 2 : -2;
			int nx = sd->x + dx;
			int ny = sd->y + dy;
			if (map_getcell(sd->m, nx, ny, CELL_CHKPASS)) {
				unit_walktoxy(sd, nx, ny, 0);
			}
		}
		return 0;
	}

	sd->bot.last_target_tick = tick;

	// ✅ [เพิ่มใหม่] จำกัดอัตราการฆ่า (อย่างน้อย 1 วินาทีต่อตัว)
	//if (tick_diff(tick, sd->bot.last_kill_tick) < 1000) {
	//	return 0;  // ยังเร็วไป ข้ามไปก่อน
	//}

	// 7. Attack / Skill (แยก CD แต่ละ slot)
	int use_skill_id = 0;
	int use_skill_lv = 0;
	int use_skill_slot = -1;
	bool skill_ready = false;

	// เช็ค CD ของสกิลแต่ละ slot (หาตัวที่พร้อมใช้)
	for (int i = 0; i < 5; i++) {
		if (sd->bot.skill_id[i] > 0 && pc_checkskill(sd, sd->bot.skill_id[i]) > 0) {
			if (tick_diff(tick, sd->bot.skill_delay_tick[i]) >= sd->bot.skill_cd[i]) {
				use_skill_id = sd->bot.skill_id[i];
				use_skill_lv = sd->bot.skill_lv[i];
				use_skill_slot = i;
				skill_ready = true;
				break;  // ✅ เจอตัวที่พร้อมใช้แล้ว ออกเลย
			}
		}
	}

	// ✅ [เพิ่มใหม่] ถ้าไม่มีสกิลไหนพร้อม และปิด auto_attack → ข้ามการโจมตี
	if (!skill_ready && !sd->bot.auto_attack) {
		sd->bot.last_action_tick = tick;
		return 0;
	}

	int range = 1;
	int inf = 0;

	if (skill_ready) {
		inf = skill_get_inf(use_skill_id);
		int actual_skill_range = skill_get_range(use_skill_id, use_skill_lv);
		if (inf & (INF_SELF_SKILL | INF_SUPPORT_SKILL)) {
			range = 3;
		}
		else {
			range = (actual_skill_range < 6) ? actual_skill_range : 6;
		}
	}
	else {
		range = status_get_range(sd);
	}

	if (current_dist <= range) {
		unit_stop_walking(sd, 1);

		if (skill_ready) {
			int cast_success = 0;

			if (inf & INF_GROUND_SKILL) {
				cast_success = unit_skilluse_pos(sd, target->x, target->y, use_skill_id, use_skill_lv);
			}
			else if (inf & (INF_SELF_SKILL | INF_SUPPORT_SKILL)) {
				cast_success = unit_skilluse_id(sd, sd->id, use_skill_id, use_skill_lv);
			}
			else {
				cast_success = unit_skilluse_id(sd, target->id, use_skill_id, use_skill_lv);
			}

			// ✅ [แก้ใหม่] ถ้าร่ายล้มเหลว + ปิด auto_attack → ไม่ทำอะไร
			if (cast_success == 0 && sd->bot.auto_attack) {
				unit_attack(sd, target->id, 1);
			}
			else if (cast_success != 0 && use_skill_slot >= 0) {
				sd->bot.skill_delay_tick[use_skill_slot] = tick;
				//sd->bot.last_kill_tick = tick;  // ✅ เพิ่ม: อัปเดตเวลาฆ่าล่าสุด
			}
			// ✅ [เพิ่ม] ถ้า cast_success == 0 และปิด auto_attack → ไม่ตี ไม่ทำอะไร
		}
		else if (sd->bot.auto_attack) {
			// ไม่มีสกิลพร้อม แต่เปิด auto_attack → ตีธรรมดา
			unit_attack(sd, target->id, 1);
		}
		// ✅ [เพิ่ม] ถ้าไม่มีสกิล + ปิด auto_attack → ยืนเฉยๆ (ไม่ตี)

		sd->bot.last_action_tick = tick;
	}
	return 0;
}

// ==========================================
// Command Handler (รับคำสั่งจาก @bot)
// ==========================================
bool autobot_handle_command(map_session_data* sd, const char* message) {
	if (!message || !*message) return false;

	char mode[50];
	memset(mode, 0, sizeof(mode));
	if (sscanf(message, "%49s", mode) < 1) return false;

	// --- ปิดบอท ---
	if (strcmpi(mode, "off") == 0) {
		autobot_cleanup(sd);
		clif_displaymessage(sd->fd, "[Autobot] Deactivated!");
		return true;
	}

	// --- [เพิ่มใหม่] ตั้งค่า Teleport Delay ---
	if (strcmpi(mode, "tp") == 0 || strcmpi(mode, "teleport") == 0) {
		int delay = 0;
		if (sscanf(message, "%*s %d", &delay) == 1) {
			if (delay < 2) delay = 2;
			sd->bot.teleport_timer = delay * 1000;
			char output[256];
			snprintf(output, sizeof output, "[Autobot] Teleport delay set to %d seconds.", delay);
			clif_displaymessage(sd->fd, output);
		}
		else {
			clif_displaymessage(sd->fd, "[Autobot] Usage: @bot tp <seconds> (min 2)");
		}
		return true;
	}

	// --- ระบบ Toggle (เปิด/ปิด อิสระ) ---
	if (strcmpi(mode, "toggle") == 0) {
		char feature[50];
		char output[256];

		if (sscanf(message, "%*s %49s", feature) == 1) {
			if (strcmpi(feature, "attack") == 0) {
				sd->bot.auto_attack = !sd->bot.auto_attack;
				snprintf(output, sizeof output, "[Autobot] Auto Attack: %s", sd->bot.auto_attack ? "ON" : "OFF");
			}
			else if (strcmpi(feature, "loot") == 0) {
				sd->bot.auto_loot = !sd->bot.auto_loot;
				sd->state.autoloot = sd->bot.auto_loot ? 10000 : 0;
				snprintf(output, sizeof output, "[Autobot] Auto Loot: %s", sd->bot.auto_loot ? "ON" : "OFF");
			}
			else if (strcmpi(feature, "buff") == 0) {
				sd->bot.auto_buff = !sd->bot.auto_buff;
				snprintf(output, sizeof output, "[Autobot] Auto Buff: %s", sd->bot.auto_buff ? "ON" : "OFF");
			}
			else if (strcmpi(feature, "potion") == 0) {
				sd->bot.auto_potion = !sd->bot.auto_potion;
				snprintf(output, sizeof output, "[Autobot] Auto Potion: %s", sd->bot.auto_potion ? "ON" : "OFF");
			}
			else if (strcmpi(feature, "itembuff") == 0) {
				sd->bot.auto_itembuff = !sd->bot.auto_itembuff;
				snprintf(output, sizeof output, "[Autobot] Auto Item Buff: %s", sd->bot.auto_itembuff ? "ON" : "OFF");
			}
			else if (strcmpi(feature, "teleport") == 0) {
				sd->bot.auto_teleport = !sd->bot.auto_teleport;
				snprintf(output, sizeof output, "[Autobot] Auto Teleport: %s", sd->bot.auto_teleport ? "ON" : "OFF");
			}
			else {
				clif_displaymessage(sd->fd, "[Autobot] Feature not found. Use: attack, loot, buff, potion, itembuff, teleport");
				return true;
			}
			clif_displaymessage(sd->fd, output);
			return true;
		}
	}

	// --- ตั้งค่า Potion ---
	if (strcmpi(mode, "potion") == 0) {
		int hp_id = 0, hp_pct = 0, sp_id = 0, sp_pct = 0;
		if (sscanf(message, "%*s %d %d %d %d", &hp_id, &hp_pct, &sp_id, &sp_pct) >= 2) {
			sd->bot.hp_item = hp_id; sd->bot.hp_pct = hp_pct;
			sd->bot.sp_item = sp_id; sd->bot.sp_pct = sp_pct;
			clif_displaymessage(sd->fd, "[Autobot] Potion settings set.");
		}
		return true;
	}

	// --- ตั้งค่า Item Buff ---
	if (strcmpi(mode, "itembuff") == 0) {
		int slot = 0, id = 0, delay = 0;
		if (sscanf(message, "%*s %d %d %d", &slot, &id, &delay) == 3) {
			if (slot >= 1 && slot <= 5) {
				if (sd->bot.buff_item_id[slot - 1] != id) {
					sd->bot.buff_item_id[slot - 1] = id;
					sd->bot.buff_item_last_tick[slot - 1] = 0;
				}
				sd->bot.buff_item_delay[slot - 1] = delay;
				clif_displaymessage(sd->fd, "[Autobot] Item Buff set.");
			}
		}
		return true;
	}

	// --- เปิดบอท (ระบบหลัก) ---
	if (strcmpi(mode, "on") == 0 || strcmpi(mode, "0") == 0) {
		int tele_sec = 0, avoid_mvp = 0;
		int s[5] = { 0 }, l[5] = { 0 };

		sscanf(message, "%*s %d %d %d %d %d %d %d %d %d %d %d %d",
			&tele_sec, &avoid_mvp, &s[0], &l[0], &s[1], &l[1], &s[2], &l[2], &s[3], &l[3], &s[4], &l[4]);

		if (tele_sec > 0 && tele_sec < 2) tele_sec = 2;
		sd->bot.teleport_timer = tele_sec * 1000;
		sd->bot.avoid_mvp = (avoid_mvp > 0);

		for (int i = 0; i < 5; i++) {
			sd->bot.skill_id[i] = s[i];
			sd->bot.skill_lv[i] = l[i] > 0 ? l[i] : 1;
			if (sd->bot.skill_cd[i] == 0) sd->bot.skill_cd[i] = 500; // Default CD 500ms
		}

		if (!sd->bot.enabled) {
			sd->bot.timer_id = add_timer_interval(gettick() + 500, autobot_timer, sd->id, 0, 500);
		}

		sd->bot.enabled = true;
		sd->bot.target_id = 0;
		sd->bot.last_action_tick = gettick();
		sd->bot.last_target_tick = gettick();
		for (int i = 0; i < 5; i++) {
			sd->bot.skill_delay_tick[i] = 0;
		}
		sd->bot.idle_walk_tick = gettick();

		// เปิด Autoloot อัตโนมัติถ้าตั้งค่า auto_loot ไว้
		if (sd->bot.auto_loot) {
			sd->state.autoloot = 10000;
		}

		clif_displaymessage(sd->fd, "[Autobot] Activated!");
		return true;
	}

	// --- ตั้งค่า Skill Active (แยก CD แต่ละ slot) ---
	if (strcmpi(mode, "skill_active") == 0) {
		int slot = 0, id = 0, lv = 0, cd = 0;
		if (sscanf(message, "%*s %d %d %d %d", &slot, &id, &lv, &cd) == 4) {
			if (slot >= 1 && slot <= 5) {
				if (cd < 300) cd = 300; // บังคับขั้นต่ำ 300 ms
				sd->bot.skill_id[slot - 1] = id;
				sd->bot.skill_lv[slot - 1] = lv;
				sd->bot.skill_cd[slot - 1] = cd;
				sd->bot.skill_delay_tick[slot - 1] = 0;
				char output[256];
				snprintf(output, sizeof output, "[Autobot] Skill Active #%d set: ID=%d LV=%d CD=%dms", slot, id, lv, cd);
				clif_displaymessage(sd->fd, output);
			}
		}
		return true;
	}

	// --- ตั้งค่า Skill Support (แยก CD แต่ละ slot) ---
	if (strcmpi(mode, "skill_support") == 0) {
		int slot = 0, id = 0, lv = 0, cd = 0;
		if (sscanf(message, "%*s %d %d %d %d", &slot, &id, &lv, &cd) == 4) {
			if (slot >= 1 && slot <= 5) {
				if (cd < 300) cd = 300; // บังคับขั้นต่ำ 300 ms
				sd->bot.buff_id[slot - 1] = id;
				sd->bot.buff_lv[slot - 1] = lv;
				sd->bot.buff_cd[slot - 1] = cd;
				sd->bot.buff_last_tick[slot - 1] = 0;
				char output[256];
				snprintf(output, sizeof output, "[Autobot] Skill Support #%d set: ID=%d LV=%d CD=%dms", slot, id, lv, cd);
				clif_displaymessage(sd->fd, output);
			}
		}
		return true;
	}

	// --- ตั้งค่า Teleport by ITEM ---
	if (strcmpi(mode, "tp_item") == 0) {
		int val = 0;
		if (sscanf(message, "%*s %d", &val) == 1) {
			sd->bot.tp_by_item = (val > 0);
			clif_displaymessage(sd->fd, sd->bot.tp_by_item ? "[Autobot] Teleport by Item: ON" : "[Autobot] Teleport by Item: OFF");
		}
		return true;
	}

	// --- ตั้งค่า Teleport by SKILL ---
	if (strcmpi(mode, "tp_skill") == 0) {
		int val = 0;
		if (sscanf(message, "%*s %d", &val) == 1) {
			sd->bot.tp_by_skill = (val > 0);
			clif_displaymessage(sd->fd, sd->bot.tp_by_skill ? "[Autobot] Teleport by Skill: ON" : "[Autobot] Teleport by Skill: OFF");
		}
		return true;
	}

	// --- ตั้งค่า Escape BOSS ---
	if (strcmpi(mode, "tp_boss") == 0) {
		int val = 0;
		if (sscanf(message, "%*s %d", &val) == 1) {
			sd->bot.escape_boss = (val > 0);
			clif_displaymessage(sd->fd, sd->bot.escape_boss ? "[Autobot] Escape Boss: ON" : "[Autobot] Escape Boss: OFF");
		}
		return true;
	}

	// --- ตั้งค่า Ignoring monster ---
	if (strcmpi(mode, "tp_ignore") == 0) {
		int mob_id = 0;
		if (sscanf(message, "%*s %d", &mob_id) == 1) {
			sd->bot.ignore_mob_id = mob_id;
			char output[256];
			snprintf(output, sizeof output, "[Autobot] Ignoring Mob ID: %d", mob_id);
			clif_displaymessage(sd->fd, output);
		}
		return true;
	}

	// --- Reset Skill Active ---
	if (strcmpi(mode, "reset_skill_active") == 0) {
		for (int i = 0; i < 5; i++) {
			sd->bot.skill_id[i] = 0;
			sd->bot.skill_lv[i] = 0;
			sd->bot.skill_cd[i] = 0;
			sd->bot.skill_delay_tick[i] = 0;
		}
		clif_displaymessage(sd->fd, "[Autobot] Skill Active slots reset.");
		return true;
	}

	// --- Reset Skill Support ---
	if (strcmpi(mode, "reset_skill_support") == 0) {
		for (int i = 0; i < 5; i++) {
			sd->bot.buff_id[i] = 0;
			sd->bot.buff_lv[i] = 0;
			sd->bot.buff_cd[i] = 0;
			sd->bot.buff_last_tick[i] = 0;
		}
		clif_displaymessage(sd->fd, "[Autobot] Skill Support slots reset.");
		return true;
	}

	// --- Reset Item Support ---
	if (strcmpi(mode, "reset_item_support") == 0) {
		for (int i = 0; i < 5; i++) {
			sd->bot.buff_item_id[i] = 0;
			sd->bot.buff_item_delay[i] = 0;
			sd->bot.buff_item_last_tick[i] = 0;
		}
		clif_displaymessage(sd->fd, "[Autobot] Item Support slots reset.");
		return true;
	}

	return false;
}
