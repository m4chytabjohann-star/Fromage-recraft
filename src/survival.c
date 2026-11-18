// survival.c - survival mode for fromage
//
// Rules are modelled on VoXide's survival code (health 20, hunger 20,
// fall damage past 3 blocks, lava, drowning after 15 s, slow regeneration
// when well fed, starvation down to 1 hp), re-written in C for fromage's
// data structures. One "tick" here is one vblank (VBLANKS_PER_SEC per second).

#include <stdio.h>
#include <sawpads.h>
#include "common.h"

#define MAX_HP   20
#define MAX_FOOD 20
#define SEC      VBLANKS_PER_SEC

#define SAFE_FALL_BLOCKS 3
#define REGEN_DELAY      (3 * SEC)
#define FOOD_DRAIN       (20 * SEC)   // one hunger point per 20 s
#define MAX_AIR          (15 * SEC)

int survival_mode = 1;
uint8_t inv_count[BLOCK_MAX];
int player_health = MAX_HP;
int player_hunger = MAX_FOOD;

// defined in main.c
extern int32_t vel_x, vel_y, vel_z;

static int regen_delay, regen_tick, food_timer, hurt_cd, air = MAX_AIR;
static int fall_peak;
static int was_grounded = 1;

static bool mining;
static int mine_x, mine_y, mine_z, mine_progress, mine_needed, mine_cd;

static int32_t spawn_x, spawn_y, spawn_z;
static bool spawn_set;

static char msg[40];
static int msg_timer;

static uint32_t drop_seed = 0x1234567;

// ---------------------------------------------------------------- messages

void survival_message(const char *s)
{
	strncpy(msg, s, sizeof(msg) - 1);
	msg[sizeof(msg) - 1] = 0;
	msg_timer = 3 * SEC;
}

void survival_set_spawn(int32_t x, int32_t y, int32_t z)
{
	spawn_x = x; spawn_y = y; spawn_z = z;
	spawn_set = true;
}

// ----------------------------------------------------------------- state

void survival_reset(void)
{
	memset(inv_count, 0, sizeof(inv_count));
	for (int i = 0; i < HOTBAR_MAX; i++) current_block[i] = 0;
	hotbar_pos = 0;
	player_health = MAX_HP;
	player_hunger = MAX_FOOD;
	regen_delay = regen_tick = food_timer = hurt_cd = 0;
	air = MAX_AIR;
	fall_peak = cam_y;
	was_grounded = 1;
	mining = false;
	mine_progress = mine_cd = 0;
	msg_timer = 0;
}

void survival_restore(int health, int hunger, const uint8_t *inv)
{
	survival_reset();
	if (health < 1) health = 1;
	if (health > MAX_HP) health = MAX_HP;
	if (hunger < 0) hunger = 0;
	if (hunger > MAX_FOOD) hunger = MAX_FOOD;
	player_health = health;
	player_hunger = hunger;
	memcpy(inv_count, inv, BLOCK_MAX);
	inv_count[0] = 0;
	// refill the hotbar from what we carry
	int slot = 0;
	for (int b = 1; b < BLOCK_MAX && slot < HOTBAR_MAX; b++) {
		if (inv_count[b] > 0) current_block[slot++] = b;
	}
}

// ------------------------------------------------------------- inventory

void survival_give(int block, int n)
{
	if (block <= 0 || block >= BLOCK_MAX || n <= 0) return;
	int v = inv_count[block] + n;
	if (v > 255) v = 255;
	inv_count[block] = v;

	for (int i = 0; i < HOTBAR_MAX; i++)
		if (current_block[i] == block) return;
	for (int i = 0; i < HOTBAR_MAX; i++) {
		int b = current_block[i];
		if (b <= 0 || b >= BLOCK_MAX || inv_count[b] == 0) {
			current_block[i] = block;
			return;
		}
	}
	// hotbar full: the item stays in the inventory, reachable with the
	// item-cycle button
}

bool survival_has_item(void)
{
	int b = current_block[hotbar_pos];
	return b > 0 && b < BLOCK_MAX && inv_count[b] > 0;
}

void survival_placed(void)
{
	int b = current_block[hotbar_pos];
	if (b <= 0 || b >= BLOCK_MAX) return;
	if (inv_count[b] > 0) inv_count[b]--;
	if (inv_count[b] == 0) current_block[hotbar_pos] = 0;
}

void survival_cycle(void)
{
	int start = current_block[hotbar_pos];
	if (start < 0) start = 0;
	for (int k = 1; k <= BLOCK_MAX; k++) {
		int b = (start + k) % BLOCK_MAX;
		if (b > 0 && inv_count[b] > 0) {
			current_block[hotbar_pos] = b;
			return;
		}
	}
	current_block[hotbar_pos] = 0;
}

// ------------------------------------------------------------ block rules

// mining time in tenths of a second, bare-handed. -1 = cannot be mined.
// (No tools yet, so stone-like blocks are much quicker than in Java.)
static int hardness_tenths(int b)
{
	switch (b) {
		case 0: case 7: case 8: case 9: case 10: case 11: return -1;
		case 6: case 37: case 38: case 39: case 40: case 46: return 0;
		case 18: return 3;
		case 20: return 4;
		case 2: case 13: case 19: return 9;
		case 3: case 12: return 8;
		case 5: case 17: return 15;
		case 47: return 18;
		case 1: case 44: case 43: return 25;
		case 4: case 45: case 48: return 30;
		case 14: case 15: case 16: return 40;
		case 41: case 42: return 50;
		case 49: return 150;
	}
	if (b >= 21 && b <= 36) return 12; // wool
	return 20;
}

static void give_drop(int b)
{
	switch (b) {
		case 1: survival_give(4, 1); break;          // stone -> cobblestone
		case 2: survival_give(3, 1); break;          // grass -> dirt
		case 18:                                     // leaves
			if (((RAND(drop_seed) >> 16) % 12) == 0) survival_give(40, 1); // "apple" stand-in: red mushroom
			break;
		case 20: break;                              // glass drops nothing
		case 43: survival_give(44, 2); break;        // double slab -> 2 slabs
		case 46: break;                              // TNT blows up instead
		default: survival_give(b, 1); break;
	}
}

// -------------------------------------------------------------- damage

static void die(void)
{
	survival_message("You died!");
	player_health = MAX_HP;
	player_hunger = MAX_FOOD;
	regen_delay = regen_tick = food_timer = hurt_cd = 0;
	air = MAX_AIR;
	mining = false;
	if (spawn_set) {
		cam_x = spawn_x; cam_y = spawn_y; cam_z = spawn_z;
	}
	vel_x = vel_y = vel_z = 0;
	fall_peak = cam_y;
	was_grounded = 1;
	// inventory is kept on death
}

void survival_hurt(int dmg)
{
	if (dmg <= 0) return;
	player_health -= dmg;
	regen_delay = REGEN_DELAY;
	if (player_health <= 0) die();
}

// --------------------------------------------------------------- mining

static void explode(int cx, int cy, int cz)
{
	for (int dx = -2; dx <= 2; dx++)
	for (int dy = -2; dy <= 2; dy++)
	for (int dz = -2; dz <= 2; dz++) {
		if (world_get_block(cx + dx, cy + dy, cz + dz) != 7)
			world_set_block(cx + dx, cy + dy, cz + dz, 0, 1);
	}
	int px = (cam_x >> 8) - cx, py = (cam_y >> 8) - cy, pz = (cam_z >> 8) - cz;
	if (ABS(px) <= 4 && ABS(py) <= 4 && ABS(pz) <= 4) survival_hurt(8);
}

void survival_mining(int mmul, bool held, int sound_on)
{
	if (mine_cd > 0) mine_cd -= mmul;

	if (!held || mine_cd > 0) {
		if (!held) { mining = false; mine_progress = 0; }
		return;
	}

	int32_t cx = -1, cy = -1, cz = -1;
	int32_t v = world_cast_ray(cam_x, cam_y, cam_z,
		mat_rt31, mat_rt32, mat_rt33, &cx, &cy, &cz, 10, false);
	if (v < 0) { mining = false; mine_progress = 0; return; }

	int bl = world_get_block(cx, cy, cz);
	int h = hardness_tenths(bl);
	if (h < 0) { mining = false; mine_progress = 0; return; }
	h = h * SEC / 10;

	if (!mining || cx != mine_x || cy != mine_y || cz != mine_z) {
		mining = true;
		mine_x = cx; mine_y = cy; mine_z = cz;
		mine_progress = 0;
	}
	mine_needed = h;
	mine_progress += mmul;

	if (mine_progress >= h) {
		if (bl == 46) {
			explode(cx, cy, cz);
		} else {
			world_set_block(cx, cy, cz, 0, 1);
			give_drop(bl);
		}
		if (!world_is_walkable(bl) && sound_on)
			sound_play(sound_get_id(bl), 0x3FFF, 0x3FFF);
		mining = false;
		mine_progress = 0;
		mine_cd = SEC / 6;
	}
}

// ---------------------------------------------------------------- eating

bool survival_eat(void)
{
	int b = current_block[hotbar_pos];
	int food = 0;
	if (b == 39 || b == 40) food = 3;      // mushrooms
	if (food == 0) { survival_message("Can't eat that"); return false; }
	if (player_hunger >= MAX_FOOD) { survival_message("Not hungry"); return false; }
	if (b < 0 || b >= BLOCK_MAX || inv_count[b] == 0) return false;
	inv_count[b]--;
	if (inv_count[b] == 0) current_block[hotbar_pos] = 0;
	player_hunger += food;
	if (player_hunger > MAX_FOOD) player_hunger = MAX_FOOD;
	return true;
}

// ---------------------------------------------------------- per-frame tick

void survival_update(int mmul, bool grounded, bool in_liquid, bool eye_in_water, bool feet_in_lava)
{
	if (msg_timer > 0) msg_timer -= mmul;
	if (hurt_cd > 0) hurt_cd -= mmul;

	// falling
	if (grounded || in_liquid) {
		if (!was_grounded && !in_liquid) {
			int blocks = (fall_peak - cam_y) >> 8;
			if (blocks > SAFE_FALL_BLOCKS) survival_hurt(blocks - SAFE_FALL_BLOCKS);
		}
		fall_peak = cam_y;
		was_grounded = 1;
	} else {
		if (cam_y > fall_peak) fall_peak = cam_y;
		was_grounded = 0;
	}

	// lava: 4 hp every half second (as in VoXide)
	if (feet_in_lava && hurt_cd <= 0) {
		hurt_cd = SEC / 2;
		survival_hurt(4);
		return;
	}

	// drowning: 15 s of air, then 2 hp per second
	if (eye_in_water) {
		if (air > 0) air -= mmul;
		else if (hurt_cd <= 0) {
			hurt_cd = SEC;
			survival_hurt(2);
			return;
		}
	} else {
		air = MAX_AIR;
	}

	// hunger drains over time
	food_timer += mmul;
	if (food_timer >= FOOD_DRAIN) {
		food_timer -= FOOD_DRAIN;
		if (player_hunger > 0) player_hunger--;
	}

	// regeneration / starvation
	if (regen_delay > 0) {
		regen_delay -= mmul;
	} else if (player_hunger == 0) {
		regen_tick += mmul;
		if (regen_tick >= 4 * SEC) {
			if (player_health > 1) player_health--;
			regen_tick = 0;
		}
	} else if (player_health < MAX_HP && player_hunger >= 18) {
		regen_tick += mmul;
		int period = (player_hunger == MAX_FOOD) ? (SEC / 2) : (4 * SEC);
		if (regen_tick >= period) {
			player_health++;
			player_hunger--;
			regen_tick = 0;
		}
	} else {
		regen_tick = 0;
	}
}

// -------------------------------------------------------------- crafting

typedef struct {
	uint8_t in1, n1, in2, n2, out, nout;
	const char *label;
} recipe_t;

#define NUM_RECIPES 8
static const recipe_t recipes[NUM_RECIPES] = {
	{17, 1,  0, 0,  5, 4, "1 Log > 4 Planks"},
	{ 4, 1,  0, 0,  1, 1, "1 Cobble > 1 Stone"},
	{12, 1,  0, 0, 20, 1, "1 Sand > 1 Glass"},
	{ 1, 3,  0, 0, 44, 6, "3 Stone > 6 Slabs"},
	{ 1, 4,  0, 0, 45, 4, "4 Stone > 4 Bricks"},
	{ 5, 6,  0, 0, 47, 1, "6 Planks > Bookshelf"},
	{12, 4, 16, 4, 46, 1, "4 Sand+4 Coal > TNT"},
	{15, 9,  0, 0, 42, 1, "9 Iron Ore > Iron Block"},
};

static int craftable(const recipe_t *r)
{
	int n = inv_count[r->in1] / r->n1;
	if (r->in2 != 0) {
		int m = inv_count[r->in2] / r->n2;
		if (m < n) n = m;
	}
	return n;
}

void survival_crafting_menu(void)
{
	static char lbl[NUM_RECIPES][40];
	int sel = 0;

	for (;;) {
		for (int i = 0; i < NUM_RECIPES; i++) {
			int n = craftable(&recipes[i]);
			if (n > 99) n = 99;
			sniprintf(lbl[i], sizeof(lbl[i]), "[%d] %s", n, recipes[i].label);
		}

		int r = gui_menu(NUM_RECIPES + 1, sel,
			lbl[0], lbl[1], lbl[2], lbl[3], lbl[4], lbl[5], lbl[6], lbl[7],
			"Back");
		if (r < 0 || r >= NUM_RECIPES) break;
		sel = r;

		const recipe_t *rc = &recipes[r];
		if (craftable(rc) > 0) {
			inv_count[rc->in1] -= rc->n1;
			if (rc->in2 != 0) inv_count[rc->in2] -= rc->n2;
			survival_give(rc->out, rc->nout);
		}
	}
}

// ------------------------------------------------------------------ HUD

static void hud_rect(int x, int y, int w, int h, uint32_t col)
{
	DMA_PUSH(3, 1);
	dma_buffer[dma_pos++] = 0x60000000 | col;
	dma_buffer[dma_pos++] = (((uint32_t)(y & 0xFFFF)) << 16) | ((uint32_t)(x & 0xFFFF));
	dma_buffer[dma_pos++] = (((uint32_t)h) << 16) | ((uint32_t)(w & 0xFFFF));
}

void survival_draw_hud(void)
{
	const int bw = 16 * VID_WIDTH_MULTIPLIER;
	const int hotbar_by = 120 - 6 - TEXT_BORDER_Y;
	const int y = hotbar_by - 8 - 11;
	const int x0 = -((bw * HOTBAR_MAX) / 2);
	const int icon_w = 7 * VID_WIDTH_MULTIPLIER;
	const int step = 8 * VID_WIDTH_MULTIPLIER;

	for (int i = 0; i < 10; i++) {
		int hx = x0 + i * step;
		int amt = player_health - 2 * i;
		hud_rect(hx, y, icon_w, 7, 0x202020);
		if (amt >= 2) hud_rect(hx, y, icon_w, 7, 0x0000D8);
		else if (amt == 1) hud_rect(hx, y, icon_w / 2, 7, 0x0000D8);
	}

	for (int k = 0; k < 10; k++) {
		int fx = -x0 - (k + 1) * step + VID_WIDTH_MULTIPLIER;
		int amt = player_hunger - 2 * k;
		hud_rect(fx, y, icon_w, 7, 0x202020);
		if (amt >= 2) hud_rect(fx, y, icon_w, 7, 0x3070D0);
		else if (amt == 1) hud_rect(fx + icon_w / 2, y, icon_w - icon_w / 2, 7, 0x3070D0);
	}

	// mining progress under the crosshair
	if (mining && mine_needed > 0) {
		int bwid = 24 * VID_WIDTH_MULTIPLIER;
		int fill = bwid * mine_progress / mine_needed;
		if (fill > bwid) fill = bwid;
		hud_rect(-bwid / 2, 12, fill, 2, 0xFFFFFF);
		hud_rect(-bwid / 2, 12, bwid, 2, 0x303030);
	}

	if (msg_timer > 0) {
		int tw = get_text_width("%s", msg);
		draw_text((VID_WIDTH - tw) / 2, 60, 0xFFFFFF, "%s", msg);
	}
}
