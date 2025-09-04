//SPDX-License-Identifier: GPL-2.1-or-later
/*
 * Copyright (C) 2023-2025 Cyril Hrubis <metan@ucw.cz>
 */

 /*

   TERMINI -- minimal terminal emulator.

  */

#include <stdio.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <errno.h>
#include <vterm.h>
#include <gfxprim.h>

#include "config.h"

#include "xterm_256_palette.h"

#define HIDE_CURSOR_TIMEOUT 1000

static gp_backend *backend;

static VTerm *vt;
static VTermScreen *vts;

static unsigned int cols;
static unsigned int rows;
static unsigned int char_width;
static unsigned int char_height;
static gp_text_style *text_style;
static gp_text_style *text_style_bold;

static gp_pixel colors[256];

static uint8_t fg_color_idx;
static uint8_t bg_color_idx;

static int focused = 0;

/* HACK to draw frames */
static void draw_utf8_frames(int x, int y, uint32_t val, gp_pixel fg)
{
	int width = (char_width+1)/2;
	int height = (char_height+1)/2;

	switch (val) {
	case 0x2500: /* Horizontal line */
		gp_hline_xyw(backend->pixmap, x, y + height, char_width, fg);
	break;
	case 0x2502: /* Vertical line */
		gp_vline_xyh(backend->pixmap, x + width, y, char_height, fg);
	break;
	case 0x250c: /* Upper left corner */
		gp_hline_xyw(backend->pixmap, x + width, y + height, width, fg);
		gp_vline_xyh(backend->pixmap, x + width, y + height, height+1, fg);
	break;
	case 0x2510: /* Upper right corner */
		gp_hline_xyw(backend->pixmap, x, y + height, width, fg);
		gp_vline_xyh(backend->pixmap, x + width, y + height, height+1, fg);
	break;
	case 0x2514: /* Bottom left corner */
		gp_hline_xyw(backend->pixmap, x + width, y + height, width, fg);
		gp_vline_xyh(backend->pixmap, x + width, y, height, fg);
	break;
	case 0x2518: /* Bottom right corner */
		gp_hline_xyw(backend->pixmap, x, y + height, width, fg);
		gp_vline_xyh(backend->pixmap, x + width, y, height+1, fg);
	break;
	case 0x251c: /* Left vertical tee */
		gp_hline_xyw(backend->pixmap, x + width, y + height, width, fg);
		gp_vline_xyh(backend->pixmap, x + width, y, char_height, fg);
	break;
	case 0x2524: /* Right vertical tee */
		gp_hline_xyw(backend->pixmap, x, y + height, width, fg);
		gp_vline_xyh(backend->pixmap, x + width, y, char_height, fg);
	break;
	default:
		fprintf(stderr, "WARN: unhandled utf8 char %x\n", val);
	}
}

static void draw_cell(VTermPos pos, int is_cursor)
{
	VTermScreenCell c;

	vterm_screen_get_cell(vts, pos, &c);

#ifdef HAVE_COLOR_INDEXED
	gp_pixel bg = colors[c.bg.indexed.idx];
	gp_pixel fg = colors[c.fg.indexed.idx];
#else
	gp_pixel bg = colors[c.bg.red];
	gp_pixel fg = colors[c.fg.red];
#endif

	if (c.attrs.reverse)
		GP_SWAP(bg, fg);

	if (is_cursor && focused)
		GP_SWAP(bg, fg);

	int x = pos.col * char_width;
	int y = pos.row * char_height;

	gp_fill_rect_xywh(backend->pixmap, x, y, char_width, char_height, bg);

	//fprintf(stderr, "Drawing %x %c %02i %02i\n", buf[0], buf[0], pos.row, pos.col);
/*
	if (c.width > 1)
		fprintf(stderr, "%i\n", c.width);
*/
	if (c.chars[0] >= 0x2500 && c.chars[0] <= 0x2524) {
		draw_utf8_frames(x, y, c.chars[0], fg);
		return;
	}

	gp_text_style *style = c.attrs.bold ? text_style_bold : text_style;

	if (c.chars[0])
		gp_glyph_draw(backend->pixmap, style, x, y, GP_TEXT_BEARING, fg, bg, c.chars[0]);

	if (is_cursor && !focused)
		gp_rect_xywh(backend->pixmap, x, y, char_width, char_height, colors[fg_color_idx]);
}

static void update_rect(VTermRect rect)
{
	int x = rect.start_col * char_width;
	int y = rect.start_row * char_height;
	int w = rect.end_col * char_width - 1;
	int h = rect.end_row * char_height - 1;

	if (rect.start_col == 0 && rect.start_row == 0 &&
	    rect.end_col == cols && rect.end_row == rows) {
		gp_backend_flip(backend);
	} else {
		gp_backend_update_rect_xyxy(backend, x, y, w, h);
	}
}

static VTermRect damaged;
static int damage_repainted = 1;

static int cursor_col;
static int cursor_row;
static int cursor_visible;
static int cursor_disable;

static void repaint_cursor(void)
{
	unsigned int x = cursor_col * char_width;
	unsigned int y = cursor_row * char_height;

	VTermPos pos = {.col = cursor_col, .row = cursor_row};

	draw_cell(pos, 1);

//	fprintf(stderr, "Painting cursor %ux%u\n", cursor_col, cursor_row);

	gp_backend_update_rect_xywh(backend, x, y, char_width, char_height);
}

static void merge_damage(VTermRect rect)
{
	if (damage_repainted) {
		damaged = rect;
		damage_repainted = 0;
		return;
	}

	damaged.start_col = GP_MIN(damaged.start_col, rect.start_col);
	damaged.end_col = GP_MAX(damaged.end_col, rect.end_col);

	damaged.start_row = GP_MIN(damaged.start_row, rect.start_row);
	damaged.end_row = GP_MAX(damaged.end_row, rect.end_row);
}

static void repaint_damage(void)
{
	int row, col;

	for (row = damaged.start_row; row < damaged.end_row; row++) {
		for (col = damaged.start_col; col < damaged.end_col; col++) {
			VTermPos pos = {.row = row, .col = col};
			draw_cell(pos, 0);
		}
	}

	if (cursor_row >= damaged.start_row && cursor_row < damaged.end_row &&
	    cursor_col >= damaged.start_col && cursor_col < damaged.end_col &&
	    cursor_visible)
		repaint_cursor();

	update_rect(damaged);
	damage_repainted = 1;
}


static int term_damage(VTermRect rect, void *user_data)
{
	(void)user_data;

	merge_damage(rect);
//	fprintf(stderr, "rect: %i %i %i %i\n", rect.start_row, rect.end_row, rect.start_col, rect.end_col);

	return 1;
}

static int term_moverect(VTermRect dest, VTermRect src, void *user_data)
{
	(void)dest;
	(void)src;
	(void)user_data;
	fprintf(stderr, "Move rect!\n");

	return 0;
}

static void clear_cursor(void)
{
	unsigned int x = cursor_col * char_width;
	unsigned int y = cursor_row * char_height;

	VTermPos pos = {.col = cursor_col, .row = cursor_row};

	draw_cell(pos, 0);

//	fprintf(stderr, "Clearing cursor %ux%u\n", cursor_col, cursor_row);

	gp_backend_update_rect_xywh(backend, x, y, char_width, char_height);
}

static int term_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user_data)
{
	(void)user_data;

	if (!cursor_visible || cursor_disable) {
		cursor_col = pos.col;
		cursor_row = pos.row;
		return 1;
	}

	clear_cursor();

	cursor_col = pos.col;
	cursor_row = pos.row;

	repaint_cursor();

	fprintf(stderr, "Move cursor %i %i -> %i %i!\n", oldpos.col, oldpos.row, pos.col, pos.row);

	//vterm_screen_flush_damage(vts);

	return 1;
}

static void term_cursor_visible(int visible)
{
	if (visible == cursor_visible)
		return;

	if (visible)
		repaint_cursor();
	else
		clear_cursor();

	cursor_visible = visible;
}

static int term_settermprop(VTermProp prop, VTermValue *val, void *user_data)
{
	(void)user_data;

	switch (prop) {
	case VTERM_PROP_TITLE:
	//	fprintf(stderr, "caption %s\n", val->string.str);
	//	gp_backend_set_caption(backend, val->string.str);
		return 1;
	case VTERM_PROP_ALTSCREEN:
		fprintf(stderr, "altscreen %i\n", val->boolean);
		return 0;
	case VTERM_PROP_ICONNAME:
	//	fprintf(stderr, "iconname %s\n", val->string.str);
		return 0;
	case VTERM_PROP_CURSORSHAPE:
		fprintf(stderr, "cursorshape %i\n", val->number);
		//TODO: HACK!
		term_cursor_visible(1);
		return 0;
	case VTERM_PROP_REVERSE:
		fprintf(stderr, "reverse %i\n", val->boolean);
		return 0;
	case VTERM_PROP_CURSORVISIBLE:
		fprintf(stderr, "cursorvisible %i\n", val->boolean);
		term_cursor_visible(val->boolean);
		return 0;
	case VTERM_PROP_CURSORBLINK:
		fprintf(stderr, "blink %i\n", val->boolean);
		return 0;
	case VTERM_PROP_MOUSE:
		fprintf(stderr, "mouse %i\n", val->number);
		return 0;
#ifdef VTERM_PROP_FOCUSREPORT
	case VTERM_PROP_FOCUSREPORT:
		fprintf(stderr, "focus report %i\n", val->boolean);
	break;
#endif
	default:
		fprintf(stderr, "PROP %i\n", prop);
	break;
	}

	fprintf(stderr, "Set term prop!\n");

	return 0;
}

static int term_screen_resize(int new_rows, int new_cols, void *user)
{
	(void)new_rows;
	(void)new_cols;
	(void)user;

	fprintf(stderr, "Resize %i %i\n", new_rows, new_cols);

	return 1;
}

static int term_bell(void *user)
{
	(void)user;
	fprintf(stderr, "Bell!\n");

	return 1;
}

static int term_sb_pushline(int cols, const VTermScreenCell *cells, void *user)
{
	(void)cols;
	(void)cells;
	(void)user;

	fprintf(stderr, "Pushline!\n");

	return 0;
}

static VTermScreenCallbacks screen_callbacks = {
	.damage      = term_damage,
//	.moverect    = term_moverect,
	.movecursor  = term_movecursor,
	.settermprop = term_settermprop,
	.bell        = term_bell,
//	.sb_pushline = term_sb_pushline,
	.resize      = term_screen_resize,
//	.sb_popline  = term_sb_popline,
};

static void term_init(void)
{
	int i;

	if (rows == 0)
		rows = 1;

	if (cols == 0)
		cols = 1;

	vt = vterm_new(rows, cols);
	vterm_set_utf8(vt, 1);

	vts = vterm_obtain_screen(vt);
	vterm_screen_enable_altscreen(vts, 1);
	vterm_screen_set_callbacks(vts, &screen_callbacks, NULL);
	VTermState *vs = vterm_obtain_state(vt);
	vterm_state_set_bold_highbright(vs, 1);

	//vterm_screen_set_damage_merge(vts, VTERM_DAMAGE_SCROLL);
	//vterm_screen_set_damage_merge(vts, VTERM_DAMAGE_ROW);

	/* We use the vterm color as an array index */
	for (i = 0; i < 16; i++) {
#ifdef HAVE_COLOR_INDEXED
		VTermColor col;
		vterm_color_indexed(&col, i);
#else
		VTermColor col = {i, i, i};
#endif
		vterm_state_set_palette_color(vs, i, &col);
	}

#ifdef HAVE_COLOR_INDEXED
	VTermColor bg, fg;

	vterm_color_indexed(&bg, bg_color_idx);
	vterm_color_indexed(&fg, fg_color_idx);
#else
	VTermColor bg = {bg_color_idx, bg_color_idx, bg_color_idx};
	VTermColor fg = {fg_color_idx, fg_color_idx, fg_color_idx};
#endif

	vterm_state_set_default_colors(vs, &fg, &bg);

	vterm_screen_reset(vts, 1);
}

/*
 * Forks and runs a shell, returns master fd.
 */
static int open_console(const char *term, const char *color)
{
	int fd, pid, flags;

	pid = forkpty(&fd, NULL, NULL, NULL);
	if (pid < 0)
		return -1;

	if (pid == 0) {
		char *shell = getenv("SHELL");

		if (!shell)
			shell = "/bin/sh";

		putenv(term);

		if (color)
			putenv(color);

		execl(shell, shell, NULL);
	}

	flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	return fd;
}

static void close_console(int fd)
{
	close(fd);
}

static void do_exit(int fd)
{
	close_console(fd);
	gp_backend_exit(backend);
	vterm_free(vt);
	exit(0);
}

static enum gp_poll_event_ret console_read(gp_fd *self)
{
	char buf[4096];
	int len;
	int fd = self->fd;

	if (cursor_visible) {
		clear_cursor();
		cursor_disable = 1;
	}

	len = read(fd, buf, sizeof(buf));
	if (len > 0)
		vterm_input_write(vt, buf, len);

	if (len < 0 && errno == EAGAIN)
		len = 0;

	if (len < 0)
		do_exit(fd);

	repaint_damage();

	if (cursor_visible) {
		cursor_disable = 0;
		repaint_cursor();
	}

	return 0;
}

static void console_write(int fd, const char *buf, int buf_len)
{
	write(fd, buf, buf_len);
}

static void term_output_callback(const char *buf, size_t len, void *usr)
{
	int fd = *(int*)usr;

	console_write(fd, buf, len);
}

static void console_resize(int fd, int cols, int rows)
{
	struct winsize size = {rows, cols, 0, 0};
	ioctl(fd, TIOCSWINSZ, &size);
}

static void key_to_console_common(gp_event *ev, int fd)
{
	switch (ev->key.key) {
	case GP_KEY_UP:
		console_write(fd, "\eOA", 3);
	break;
	case GP_KEY_DOWN:
		console_write(fd, "\eOB", 3);
	break;
	case GP_KEY_RIGHT:
		console_write(fd, "\eOC", 3);
	break;
	case GP_KEY_LEFT:
		console_write(fd, "\eOD", 3);
	break;
	case GP_KEY_INSERT:
		console_write(fd, "\e[2~", 4);
	break;
	case GP_KEY_DELETE:
		console_write(fd, "\e[3~", 4);
	break;
	case GP_KEY_PAGE_UP:
		console_write(fd, "\e[5~", 4);
	break;
	case GP_KEY_PAGE_DOWN:
		console_write(fd, "\e[6~", 4);
	break;
	case GP_KEY_F1:
		console_write(fd, "\e[11~", 5);
	break;
	case GP_KEY_F2:
		console_write(fd, "\e[12~", 5);
	break;
	case GP_KEY_F3:
		console_write(fd, "\e[13~", 5);
	break;
	case GP_KEY_F4:
		console_write(fd, "\e[14~", 5);
	break;
	case GP_KEY_F5:
		console_write(fd, "\e[15~", 5);
	break;
	case GP_KEY_F6:
		console_write(fd, "\e[17~", 5);
	break;
	case GP_KEY_F7:
		console_write(fd, "\e[18~", 5);
	break;
	case GP_KEY_F8:
		console_write(fd, "\e[19~", 5);
	break;
	case GP_KEY_F9:
		console_write(fd, "\e[20~", 5);
	break;
	case GP_KEY_F10:
		console_write(fd, "\e[21~", 5);
	break;
	case GP_KEY_F11:
		console_write(fd, "\e[23~", 5);
	break;
	case GP_KEY_F12:
		console_write(fd, "\e[24~", 5);
	break;
	}
}

static void key_to_console_xterm(gp_event *ev, int fd)
{
	key_to_console_common(ev, fd);

	switch (ev->key.key) {
	case GP_KEY_HOME:
		console_write(fd, "\eOH", 3);
	break;
	case GP_KEY_END:
		console_write(fd, "\eOF", 3);
	break;
	}
}

static void key_to_console_xterm_r5(gp_event *ev, int fd)
{
	key_to_console_common(ev, fd);

	switch (ev->key.key) {
	case GP_KEY_HOME:
		console_write(fd, "\E[1~", 4);
	break;
	case GP_KEY_END:
		console_write(fd, "\E[4~", 4);
	break;
	}
}

static void utf_to_console(gp_event *ev, int fd)
{
	char utf_buf[4];
	int bytes = gp_to_utf8(ev->utf.ch, utf_buf);

	console_write(fd, utf_buf, bytes);
}

static void init_colors_rgb(gp_backend *backend)
{
	size_t i;

	for (i = 0; i < GP_ARRAY_SIZE(colors); i++) {
		colors[i] = gp_rgb_to_pixmap_pixel(RGB_colors[i].r,
		                                   RGB_colors[i].g,
		                                   RGB_colors[i].b,
		                                   backend->pixmap);
	}
}

/*
 * Maps background white (black) and everything else black (white) that
 * produces most readable output for monochrome.
 */
static void init_colors_1bpp(gp_backend *backend, int reverse)
{
	int i;

	gp_pixel black = gp_rgb_to_pixmap_pixel(0x00, 0x00, 0x00, backend->pixmap);
	gp_pixel white = gp_rgb_to_pixmap_pixel(0xff, 0xff, 0xff, backend->pixmap);

	for (i = 0; i < 16; i++) {
		if (i == bg_color_idx)
			colors[i] = reverse ? black : white;
		else
			colors[i] = reverse ? white : black;
	}
}

/*
 * Maps background white (black) and foreground black (white), bright colors to
 * light_gray (dark_gray) and dark colors to dark_gray (light_gray).
 */
static void init_colors_2bpp(gp_backend *backend, int reverse)
{
	int i;

	gp_pixel black = gp_rgb_to_pixmap_pixel(0x00, 0x00, 0x00, backend->pixmap);
	gp_pixel dark_gray = gp_rgb_to_pixmap_pixel(0x40, 0x40, 0x40, backend->pixmap);
	gp_pixel light_gray = gp_rgb_to_pixmap_pixel(0x80, 0x80, 0x80, backend->pixmap);
	gp_pixel white = gp_rgb_to_pixmap_pixel(0xff, 0xff, 0xff, backend->pixmap);

	for (i = 0; i < 8; i++)
		colors[i] = reverse ? dark_gray : light_gray;

	for (i = 8; i < 16; i++)
		colors[i] = reverse ? light_gray : dark_gray;

	colors[fg_color_idx] = reverse ? white : black;
	colors[bg_color_idx] = reverse ? black : white;
}

static int cursor_hidden;

static uint32_t hide_cursor(gp_timer *self)
{
	gp_backend_cursor_set(backend, GP_BACKEND_CURSOR_HIDE);

	cursor_hidden = 1;

	return GP_TIMER_STOP;
}

static gp_timer hide_cursor_timer = {
	.callback = hide_cursor,
	.id = "Hide Cursor",
	.expires = HIDE_CURSOR_TIMEOUT,
};

static void hide_cursor_reschedule(void)
{
	if (gp_timer_running(&hide_cursor_timer))
		gp_backend_timer_rem(backend, &hide_cursor_timer);

	if (cursor_hidden) {
		cursor_hidden = 0;
		gp_backend_cursor_set(backend, GP_BACKEND_CURSOR_SHOW);
	}

	hide_cursor_timer.expires = HIDE_CURSOR_TIMEOUT;

	gp_backend_timer_add(backend, &hide_cursor_timer);
}

static void clipboard_to_console(int fd)
{
	char *clipboard, *c;

	clipboard = gp_backend_clipboard_get(backend);
	if (!clipboard)
		return;

	console_write(fd, clipboard, strlen(clipboard));

	free(clipboard);
}

static void backend_init(const char *backend_opts, int reverse)
{
	backend = gp_backend_init(backend_opts, 0, 0, "Termini");
	if (!backend) {
		fprintf(stderr, "Failed to initalize backend\n");
		exit(1);
	}

	if (reverse) {
		bg_color_idx = 0;
		fg_color_idx = 7;
	} else {
		fg_color_idx = 0;
		bg_color_idx = 15;
	}

	switch (gp_pixel_size(backend->pixmap->pixel_type)) {
	case 1:
		init_colors_1bpp(backend, reverse);
	break;
	case 2:
		init_colors_2bpp(backend, reverse);
	break;
	default:
		init_colors_rgb(backend);
	}

	gp_backend_cursor_set(backend, GP_BACKEND_CURSOR_TEXT_EDIT);
	gp_backend_timer_add(backend, &hide_cursor_timer);
}

static void print_help(const char *name, int exit_val)
{
	gp_fonts_iter i;
	const gp_font_family *f;

	printf("usage: %s [-r] [-b backend_opts] [-F font_family]\n\n", name);

	printf(" -b backend init string (pass -b help for options)\n");
	printf(" -r reverse colors\n");
	printf(" -F gfpxrim font family\n");
	printf("    Available fonts families:\n");
	GP_FONT_FAMILY_FOREACH(&i, f)
		printf("\t - %s\n", f->family_name);

	exit(exit_val);
}

/*
 * Emulate vt220 for monochrome and grayscale, that limits most of the
 * applications from using colors in a way that produce an unreadable output
 * while it still makes most console functions keys compatible with xterm.
 *
 * However there are still applications that produce colors without checking
 * the terminal capabilities for these we handpick color mapping for 1bpp and
 * 2bpp below.
 */
int main(int argc, char *argv[])
{
	int opt;
	const char *backend_opts = NULL;
	const char *font_family = "haxor-narrow-18";
	const char *color_fg_bg = NULL;
	const gp_font_family *ffamily;
	int reverse = 0;
	int is_grayscale;
	const char *color = NULL;;

	while ((opt = getopt(argc, argv, "b:F:hr")) != -1) {
		switch (opt) {
		case 'b':
			backend_opts = optarg;
		break;
		case 'F':
			font_family = optarg;
		break;
		case 'h':
			print_help(argv[0], 0);
		break;
		case 'r':
			reverse = 1;
			/* libvterm does not implement xterm specific CSI to get fg/bg */
			color_fg_bg="COLORFGBG=7;0";
		break;
		default:
			print_help(argv[0], 1);
		}
	}

	ffamily = gp_font_family_lookup(font_family);
	if (!ffamily) {
		fprintf(stderr, "Error; Font family %s not found!\n\n", font_family);
		print_help(argv[0], 1);
	}

	gp_text_style style = {
		.font = gp_font_family_face_lookup(ffamily, GP_FONT_MONO),
	        .pixel_xmul = 1,
		.pixel_ymul = 1,
	};

	gp_text_style style_bold = {
		.font = gp_font_family_face_lookup(ffamily, GP_FONT_MONO | GP_FONT_BOLD),
	        .pixel_xmul = 1,
		.pixel_ymul = 1,
	};

	text_style = &style;
	text_style_bold = &style_bold;

	char_width  = gp_text_max_width(text_style, 1);
	char_height = gp_text_height(text_style);

	backend_init(backend_opts, reverse);

	is_grayscale = gp_pixel_size(backend->pixmap->pixel_type) <= 4;

	cols = gp_pixmap_w(backend->pixmap)/char_width;
	rows = gp_pixmap_h(backend->pixmap)/char_height;

	fprintf(stderr, "Cols %i Rows %i\n", cols, rows);

	term_init();

	int fd;

	if (is_grayscale)
		fd = open_console("TERM=xterm-r5", color_fg_bg);
	else
		fd = open_console("TERM=xterm", color_fg_bg);

	vterm_output_set_callback(vt, term_output_callback, &fd);

	gp_fd pfd = {
		.fd = fd,
		.event = console_read,
		.events = GP_POLLIN,
		.priv = backend,
	};

	gp_backend_poll_add(backend, &pfd);
	console_resize(fd, cols, rows);

	gp_fill(backend->pixmap, colors[bg_color_idx]);

	for (;;) {
		gp_event *ev;

		while ((ev = gp_backend_ev_wait(backend))) {
			//gp_ev_dump(ev);
			switch (ev->type) {
			case GP_EV_KEY:
				if (ev->code == GP_EV_KEY_UP)
					break;

				if (ev->val == GP_BTN_MIDDLE) {
					gp_backend_clipboard_request(backend);
					continue;
				}

				if (is_grayscale)
					key_to_console_xterm_r5(ev, fd);
				else
					key_to_console_xterm(ev, fd);
			break;
			case GP_EV_UTF:
				utf_to_console(ev, fd);
			break;
			case GP_EV_REL:
			case GP_EV_ABS:
				hide_cursor_reschedule();
			break;
			case GP_EV_SYS:
				switch (ev->code) {
				case GP_EV_SYS_RESIZE:
					gp_backend_resize_ack(backend);
					cols = GP_MAX(1u, ev->sys.w/char_width);
					rows = GP_MAX(1u, ev->sys.h/char_height);
					vterm_set_size(vt, rows, cols);
					console_resize(fd, cols, rows);
					gp_fill(backend->pixmap, colors[bg_color_idx]);
					VTermRect rect = {.start_row = 0, .start_col = 0, .end_row = rows, .end_col = cols};
					term_damage(rect, NULL);
					repaint_damage();
				break;
				case GP_EV_SYS_QUIT:
					do_exit(fd);
				break;
				case GP_EV_SYS_CLIPBOARD:
					clipboard_to_console(fd);
				break;
				case GP_EV_SYS_FOCUS:
					focused = ev->val;
					if (cursor_visible)
						repaint_cursor();
				break;
				}
			break;
			}
		}
	}

	return 0;
}
