#ifndef AUTOBOT_HPP
#define AUTOBOT_HPP

#include "map.hpp"

// ==========================================
// โครงสร้างข้อมูลสำหรับ Autobot (AI Bot)
// ==========================================
struct autobot_data {
	// สถานะหลัก
	bool enabled = false;
	int timer_id = 0;

	// Targeting & Movement
	int target_id = 0;
	t_tick last_action_tick = 0;
	t_tick last_target_tick = 0;
	t_tick last_move_tick = 0;
	int last_x = 0;
	int last_y = 0;
	t_tick idle_walk_tick = 0;
	t_tick dead_tick = 0;
	t_tick last_kill_tick = 0;  // สำหรับแก้ event queue เต็ม

	// --- ระบบ Toggle (เปิด/ปิด อิสระ) ---
	bool auto_attack = true;     // โจมตีธรรมดา
	bool auto_loot = true;       // เก็บไอเท็ม
	bool auto_buff = true;       // สกิลบัพ
	bool auto_potion = true;     // กินยา HP/SP
	bool auto_itembuff = true;   // ไอเท็มบัพ
	bool auto_teleport = true;   // เทเลพอร์ต
	bool avoid_mvp = false;
	bool flee_mvp = false;

	// Teleport Options (แยกประเภท)
	bool tp_by_item = true;      // วิงโดยไอเท็ม
	bool tp_by_skill = true;     // วิงโดยสกิล
	bool escape_boss = false;    // หนีบอส
	int ignore_mob_id = 0;       // Mob ID ที่ต้องการเพิกเฉย

	// Teleport
	int teleport_timer = 0;

	// Attack Skills (แยก CD แต่ละ slot)
	uint16 skill_id[5] = { 0 };
	uint16 skill_lv[5] = { 0 };
	int skill_cd[5] = { 0 };             // CD ของแต่ละสกิล (ms)
	t_tick skill_delay_tick[5] = { 0 };  // เวลาที่ใช้สกิลล่าสุดของแต่ละ slot

	// Buff Skills (แยก CD แต่ละ slot)
	uint16 buff_id[5] = { 0 };
	uint16 buff_lv[5] = { 0 };
	int buff_cd[5] = { 0 };              // CD ของแต่ละสกิล (ms)
	t_tick buff_last_tick[5] = { 0 };    // เวลาที่ใช้สกิลล่าสุดของแต่ละ slot

	// Potions
	uint16 hp_item = 0;
	int hp_pct = 0;
	uint16 sp_item = 0;
	int sp_pct = 0;
	t_tick potion_delay_tick = 0;

	// Item Buffs
	uint16 buff_item_id[5] = { 0 };
	int buff_item_delay[5] = { 0 };
	t_tick buff_item_last_tick[5] = { 0 };
};

// ฟังก์ชันหลักของ Autobot
void autobot_init(map_session_data* sd);
void autobot_cleanup(map_session_data* sd);
bool autobot_handle_command(map_session_data* sd, const char* message);
TIMER_FUNC(autobot_timer);

#endif // AUTOBOT_HPP
