#include <stdint.h>
#include <stddef.h>

#define SYS_YIELD 7
#define SYS_EXIT 8
#define SYS_VIDEO_INFO 9
#define SYS_VIDEO_DRAW 11
#define SYS_VIDEO_BEGIN_FRAME 14
#define SYS_VIDEO_PRESENT 16
#define SYS_KEYBOARD_POLL 17

#define KEY_ARROW_UP 0x100
#define KEY_ARROW_DOWN 0x101
#define KEY_ARROW_LEFT 0x102
#define KEY_ARROW_RIGHT 0x103

#define VIDEO_OP_CELL 1
#define DRAW_BATCH_CAPACITY 512

#define SNAKE_CAPACITY 4096
#define SNAKE_MASK (SNAKE_CAPACITY - 1)

#define SNAKE_TICK_YIELDS 900
#define SNAKE_TICK_MIN 420
#define SNAKE_TICK_STEP 8

#define INITIAL_LENGTH 3
#define POINTS_PER_FOOD 10
#define INPUT_QUEUE_SIZE 3

enum { STATE_PLAYING = 0, STATE_PAUSED = 1, STATE_OVER = 2, STATE_WON = 3 };

typedef struct {
	int framebuffer_ready;
	int columns;
	int rows;
	int pixel_width;
	int pixel_height;
} video_info_t;

typedef struct {
	uint32_t type;
	int x;
	int y;
	uint32_t value;
	uint8_t alpha;
	uint8_t color;
	char character;
	uint32_t aux;
	uint32_t aux2;
} video_op_t;

__attribute__((optimize("no-tree-loop-distribute-patterns")))
void *memset(void *dest, int value, size_t count) {
	uint8_t *p = (uint8_t *)dest;
	while (count--) *p++ = (uint8_t)value;
	return dest;
}

__attribute__((optimize("no-tree-loop-distribute-patterns")))
void *memcpy(void *dest, const void *src, size_t count) {
	uint8_t *d = (uint8_t *)dest;
	const uint8_t *s = (const uint8_t *)src;
	while (count--) *d++ = *s++;
	return dest;
}

static video_op_t draw_batch[DRAW_BATCH_CAPACITY];
static uint32_t draw_batch_count;

static uint16_t snake_ring[SNAKE_CAPACITY];
static uint8_t occupied[SNAKE_CAPACITY];
static uint32_t snake_head;
static int snake_length;

static int screen_columns;
static int screen_rows;
static int board_width;
static int board_height;
static int board_x;
static int board_y;
static int food_cell = -1;
static int direction_x;
static int direction_y;

static int queue_dx[INPUT_QUEUE_SIZE];
static int queue_dy[INPUT_QUEUE_SIZE];
static int queue_count;

static uint32_t random_state = 0x91e10da5U;
static uint32_t entropy_counter;
static uint32_t high_score;

static inline int piux_call(uint32_t number, uint32_t first,
							uint32_t second, uint32_t third) {
	int result;
	asm volatile ("int $0x80"
				  : "=a"(result)
				  : "a"(number), "b"(first), "c"(second), "d"(third)
				  : "memory");
	return result;
}

static void flush_draw_batch(void) {
	if (!draw_batch_count) return;
	piux_call(SYS_VIDEO_DRAW, (uint32_t)(uintptr_t)draw_batch,
			  draw_batch_count, 0);
	draw_batch_count = 0;
}

static void put_cell(int x, int y, char character, uint8_t color) {
	video_op_t *operation;
	if (x < 0 || y < 0 || x >= screen_columns || y >= screen_rows) return;
	if (draw_batch_count == DRAW_BATCH_CAPACITY) flush_draw_batch();
	operation = &draw_batch[draw_batch_count++];
	operation->type = VIDEO_OP_CELL;
	operation->x = x;
	operation->y = y;
	operation->value = 0;
	operation->alpha = 0;
	operation->color = color;
	operation->character = character;
	operation->aux = 0;
	operation->aux2 = 0;
}

static void draw_text(int x, int y, const char *text, uint8_t color) {
	while (*text) put_cell(x++, y, *text++, color);
}

static void draw_number(int x, int y, uint32_t value, uint8_t color) {
	char digits[10];
	int count = 0;
	do {
		digits[count++] = (char)('0' + value % 10);
		value /= 10;
	} while (value && count < 10);
	while (count) put_cell(x++, y, digits[--count], color);
}

static uint32_t next_random(void) {
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return random_state;
}

static uint32_t snake_cell_at_tail(void) {
	return snake_ring[(snake_head - (uint32_t)snake_length + 1) & SNAKE_MASK];
}

static void place_food(void) {
	uint32_t cells = (uint32_t)(board_width * board_height);
	uint32_t free_cells = cells - (uint32_t)snake_length;
	uint32_t target;
	if ((uint32_t)snake_length >= cells) {
		food_cell = -1;
		return;
	}
	target = next_random() % free_cells;
	for (uint32_t cell = 0; cell < cells; cell++) {
		if (occupied[cell]) continue;
		if (target == 0) {
			food_cell = (int)cell;
			return;
		}
		target--;
	}
	food_cell = -1;
}

static void start_game(void) {
	int start_x = board_width / 2;
	int start_y = board_height / 2;

	memset(occupied, 0, sizeof(occupied));
	snake_length = INITIAL_LENGTH;
	snake_head = INITIAL_LENGTH - 1;
	for (int index = 0; index < INITIAL_LENGTH; index++) {
		int cell = start_y * board_width + (start_x - (INITIAL_LENGTH - 1) + index);
		snake_ring[index] = (uint16_t)cell;
		occupied[cell] = 1;
	}
	direction_x = 1;
	direction_y = 0;
	queue_count = 0;

	random_state ^= entropy_counter * 2654435761U;
	if (!random_state) random_state = 0x91e10da5U;
	place_food();
}

static uint32_t current_score(void) {
	return (uint32_t)(snake_length - INITIAL_LENGTH) * POINTS_PER_FOOD;
}

static uint32_t tick_yields(void) {
	uint32_t eaten = (uint32_t)(snake_length - INITIAL_LENGTH);
	uint32_t reduction = eaten * SNAKE_TICK_STEP;
	if (reduction > SNAKE_TICK_YIELDS - SNAKE_TICK_MIN)
		reduction = SNAKE_TICK_YIELDS - SNAKE_TICK_MIN;
	return SNAKE_TICK_YIELDS - reduction;
}

static void render(int state) {
	int status_y = board_y + board_height + 1;

	piux_call(SYS_VIDEO_BEGIN_FRAME, 0, 0, 0);
	for (int y = 0; y < screen_rows; y++) {
		for (int x = 0; x < screen_columns; x++) put_cell(x, y, ' ', 0x07);
	}

	draw_text(2, 0, "SNAKE", 0x0b);
	if (screen_columns >= 80)
		draw_text(18, 0, "WASD / ARROWS: MOVE   P: PAUSE   Q: QUIT", 0x07);
	else
		draw_text(14, 0, "WASD/P/Q", 0x07);

	if (screen_columns >= 48) {
		draw_text(screen_columns - 20, 0, "SC:", 0x07);
		draw_number(screen_columns - 17, 0, current_score(), 0x0e);
		draw_text(screen_columns - 10, 0, "HI:", 0x07);
		draw_number(screen_columns - 7, 0,
					high_score > current_score() ? high_score : current_score(), 0x0d);
	} else if (screen_columns >= 32) {
		draw_text(screen_columns - 10, 0, "SC:", 0x07);
		draw_number(screen_columns - 7, 0, current_score(), 0x0e);
	}

	for (int x = -1; x <= board_width; x++) {
		put_cell(board_x + x, board_y - 1, '#', 0x0a);
		put_cell(board_x + x, board_y + board_height, '#', 0x0a);
	}
	for (int y = 0; y < board_height; y++) {
		put_cell(board_x - 1, board_y + y, '#', 0x0a);
		put_cell(board_x + board_width, board_y + y, '#', 0x0a);
	}

	for (int index = 0; index < snake_length; index++) {
		uint32_t cell = snake_ring[(snake_head - (uint32_t)(snake_length - 1 - index)) & SNAKE_MASK];
		int is_head = index == snake_length - 1;
		put_cell(board_x + (int)(cell % (uint32_t)board_width),
				 board_y + (int)(cell / (uint32_t)board_width),
				 is_head ? '@' : 'o', is_head ? 0x0f : ((index & 1) ? 0x02 : 0x0a));
	}
	if (food_cell >= 0) {
		put_cell(board_x + food_cell % board_width,
				 board_y + food_cell / board_width, '*', 0x0c);
	}

	switch (state) {
	case STATE_WON:
		if (screen_columns >= 38)
			draw_text(2, status_y, "YOU WIN!  R: RESTART  Q: QUIT", 0x0a);
		else
			draw_text(2, status_y, "WIN R/Q", 0x0a);
		break;
	case STATE_OVER:
		if (screen_columns >= 38)
			draw_text(2, status_y, "GAME OVER  R: RESTART  Q: QUIT", 0x0c);
		else
			draw_text(2, status_y, "OVER R/Q", 0x0c);
		break;
	case STATE_PAUSED:
		draw_text(2, status_y, "PAUSED  P: RESUME  Q: QUIT", 0x0e);
		break;
	default:
		draw_text(2, status_y, "EAT * TO GROW", 0x07);
		if (screen_columns >= 32) {
			draw_text(screen_columns - 11, status_y, "LEN:", 0x07);
			draw_number(screen_columns - 7, status_y, (uint32_t)snake_length, 0x0e);
		}
		break;
	}

	flush_draw_batch();
	piux_call(SYS_VIDEO_PRESENT, 0, 0, 0);
}

static void queue_direction(int key) {
	int dx = 0, dy = 0;
	int last_dx = queue_count ? queue_dx[queue_count - 1] : direction_x;
	int last_dy = queue_count ? queue_dy[queue_count - 1] : direction_y;

	if (key == 'w' || key == 'W' || key == KEY_ARROW_UP) dy = -1;
	else if (key == 's' || key == 'S' || key == KEY_ARROW_DOWN) dy = 1;
	else if (key == 'a' || key == 'A' || key == KEY_ARROW_LEFT) dx = -1;
	else if (key == 'd' || key == 'D' || key == KEY_ARROW_RIGHT) dx = 1;
	else return;

	if (dx == last_dx && dy == last_dy) return;
	if (dx == -last_dx && dy == -last_dy) return;
	if (queue_count == INPUT_QUEUE_SIZE) return;

	queue_dx[queue_count] = dx;
	queue_dy[queue_count] = dy;
	queue_count++;
}

static void apply_queued_direction(void) {
	if (!queue_count) return;
	direction_x = queue_dx[0];
	direction_y = queue_dy[0];
	for (int index = 1; index < queue_count; index++) {
		queue_dx[index - 1] = queue_dx[index];
		queue_dy[index - 1] = queue_dy[index];
	}
	queue_count--;
}

static int advance_game(void) {
	int next_x, next_y, next_cell, eating;
	uint32_t tail_cell;

	apply_queued_direction();

	next_x = (int)(snake_ring[snake_head & SNAKE_MASK] % (uint32_t)board_width) + direction_x;
	next_y = (int)(snake_ring[snake_head & SNAKE_MASK] / (uint32_t)board_width) + direction_y;
	if (next_x < 0 || next_x >= board_width ||
		next_y < 0 || next_y >= board_height) return -1;

	next_cell = next_y * board_width + next_x;
	eating = next_cell == food_cell;
	tail_cell = snake_cell_at_tail();

	if (occupied[next_cell] && (eating || (uint32_t)next_cell != tail_cell)) return -1;

	if (!eating) occupied[tail_cell] = 0;
	else snake_length++;

	snake_head = (snake_head + 1) & SNAKE_MASK;
	snake_ring[snake_head] = (uint16_t)next_cell;
	occupied[next_cell] = 1;

	if (eating) {
		if (current_score() > high_score) high_score = current_score();
		place_food();
		if (food_cell < 0) return 1;
	}
	return 0;
}

static void quit_game(void) {
	piux_call(SYS_EXIT, 0, 0, 0);
	for (;;) { }
}

void _start(void) {
	video_info_t info;
	int restart;

	if (piux_call(SYS_VIDEO_INFO, (uint32_t)(uintptr_t)&info, 0, 0) < 0 ||
		info.columns < 24 || info.rows < 10) {
		piux_call(SYS_EXIT, 1, 0, 0);
		for (;;) { }
	}

	screen_columns = info.columns;
	screen_rows = info.rows;
	board_width = info.columns - 4;
	board_height = info.rows - 4;
	if (board_width > 96) board_width = 96;
	if (board_height > 42) board_height = 42;
	if (board_width * board_height > SNAKE_CAPACITY)
		board_height = SNAKE_CAPACITY / board_width;
	board_x = (info.columns - board_width) / 2;
	board_y = 2;

	do {
		uint32_t frame_wait = 0;
		int state = STATE_PLAYING;
		int result;
		restart = 0;
		start_game();
		render(state);

		for (;;) {
			int key;
			int dirty = 0;
			while ((key = piux_call(SYS_KEYBOARD_POLL, 0, 0, 0)) >= 0) {
				entropy_counter += (uint32_t)key + 1;
				if (key == 'q' || key == 'Q') quit_game();
				if (key == 'p' || key == 'P') {
					state = state == STATE_PAUSED ? STATE_PLAYING : STATE_PAUSED;
					dirty = 1;
					continue;
				}
				if (state == STATE_PLAYING) queue_direction(key);
			}
			if (dirty) render(state);

			piux_call(SYS_YIELD, 0, 0, 0);
			entropy_counter++;
			if (state == STATE_PAUSED) continue;
			if (++frame_wait < tick_yields()) continue;
			frame_wait = 0;

			result = advance_game();
			state = result < 0 ? STATE_OVER : (result > 0 ? STATE_WON : STATE_PLAYING);
			render(state);
			if (result != 0) break;
		}

		for (;;) {
			int key = piux_call(SYS_KEYBOARD_POLL, 0, 0, 0);
			if (key == 'q' || key == 'Q') quit_game();
			if (key == 'r' || key == 'R') {
				restart = 1;
				break;
			}
			piux_call(SYS_YIELD, 0, 0, 0);
			entropy_counter++;
		}
	} while (restart);

	quit_game();
}