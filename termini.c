//SPDX-License-Identifier: GPL-2.1-or-later
/*
 * Copyright (C) 2023 Cyril Hrubis <metan@ucw.cz>
 */

 /*

   TERMINI -- minimal terminal emulator.

  */

#include <stdio.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <vterm.h>
#include <gfxprim.h>

#include "config.h"

static gp_backend *backend;

static VTerm *vt;
static VTermScreen *vts;

static unsigned int cols;
static unsigned int rows;
static unsigned int char_width;
static unsigned int char_height;
static gp_text_style *text_style;
static gp_text_style *text_style_bold;

static gp_pixel colors[16];

/* delay before we repaint merged damage */
static int repaint_sleep_ms = -1;

/* HACK to draw frames */
static void draw_utf8_frames(int x, int y, uint32_t val, gp_pixel fg)
{
	switch (val) {
	case 0x2500: /* Horizontal line */
		gp_hline_xyw(backend->pixmap, x, y + char_height/2, char_width, fg);
	break;
	case 0x2502: /* Vertical line */
		gp_vline_xyh(backend->pixmap, x + char_width/2, y, char_height, fg);
	break;
	case 0x250c: /* Upper left corner */
		gp_hline_xyw(backend->pixmap, x + char_width/2, y + char_height/2, char_width/2, fg);
		gp_vline_xyh(backend->pixmap, x + char_width/2, y + char_height/2, char_height/2+1, fg);
	break;
	case 0x2510: /* Upper right corner */
		gp_hline_xyw(backend->pixmap, x, y + char_height/2, char_width/2, fg);
		gp_vline_xyh(backend->pixmap, x + char_width/2, y + char_height/2, char_height/2+1, fg);
	break;
	case 0x2514: /* Bottom left corner */
		gp_hline_xyw(backend->pixmap, x + char_width/2, y + char_height/2, char_width/2, fg);
		gp_vline_xyh(backend->pixmap, x + char_width/2, y, char_height/2, fg);
	break;
	case 0x2518: /* Bottom right corner */
		gp_hline_xyw(backend->pixmap, x, y + char_height/2, char_width/2, fg);
		gp_vline_xyh(backend->pixmap, x + char_width/2, y, char_height/2+1, fg);
	break;
	case 0x251c: /* Left vertical tee */
		gp_hline_xyw(backend->pixmap, x + char_width/2, y + char_height/2, char_width/2, fg);
		gp_vline_xyh(backend->pixmap, x + char_width/2, y, char_height, fg);
	break;
	case 0x2524: /* Right vertical tee */
		gp_hline_xyw(backend->pixmap, x, y + char_height/2, char_width/2, fg);
		gp_vline_xyh(backend->pixmap, x + char_width/2, y, char_height, fg);
	break;
	default:
		fprintf(stderr, "WARN: unhandled utf8 char %x\n", val);
	}
}

static void draw_cell(VTermPos pos)
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

	int x = pos.col * char_width;
	int y = pos.row * char_height;

	gp_fill_rect_xywh(backend->pixmap, x, y, char_width, char_height, bg);

	//fprintf(stderr, "Drawing %x %c %02i %02i\n", buf[0], buf[0], pos.row, pos.col);
/*
	if (c.width > 1)
		fprintf(stderr, "%i\n", c.width);
*/
	if (c.chars[0] >= 0x2500) {
		draw_utf8_frames(x, y, c.chars[0], fg);
		return;
	}

	gp_text_style *style = c.attrs.bold ? text_style_bold : text_style;

	char buf[5] = {};

	gp_to_utf8(c.chars[0], buf);

	gp_text(backend->pixmap, style, x, y, GP_ALIGN_RIGHT | GP_VALIGN_BELOW, fg, bg, buf);
}

static void update_rect(VTermRect rect)
{
	int x = rect.start_col * char_width;
	int y = rect.start_row * char_height;
	int w = rect.end_col * char_width;
	int h = rect.end_row * char_height;

	gp_backend_update_rect_xyxy(backend, x, y, w, h);
}

static VTermRect damaged;
static int damage_repainted = 1;

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
			draw_cell(pos);
		}
	}

	update_rect(damaged);
	damage_repainted = 1;
	repaint_sleep_ms = -1;
}


static int term_damage(VTermRect rect, void *user_data)
{
	(void)user_data;

	merge_damage(rect);
//	fprintf(stderr, "rect: %i %i %i %i\n", rect.start_row, rect.end_row, rect.start_col, rect.end_col);
	repaint_sleep_ms = 1;

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

static int term_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user_data)
{
	(void)user_data;
	unsigned int x = oldpos.col * char_width;
	unsigned int y = oldpos.row * char_height;

	draw_cell(oldpos);
	gp_backend_update_rect_xywh(backend, x, y, char_width, char_height);

	x = pos.col * char_width;
	y = pos.row * char_height;

	gp_rect_xywh(backend->pixmap, x, y, char_width, char_height, 0xffffff);
	gp_backend_update_rect_xywh(backend, x, y, char_width, char_height);

	//fprintf(stderr, "Move cursor %i %i -> %i %i!\n", oldpos.col, oldpos.row, pos.col, pos.row);

	//vterm_screen_flush_damage(vts);

	return 1;
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
		fprintf(stderr, "altscreen\n");
		return 0;
	case VTERM_PROP_ICONNAME:
	//	fprintf(stderr, "iconname %s\n", val->string.str);
		return 0;
	case VTERM_PROP_CURSORSHAPE:
		fprintf(stderr, "cursorshape %i\n", val->number);
		return 0;
	case VTERM_PROP_REVERSE:
		fprintf(stderr, "reverse %i\n", val->boolean);
		return 0;
	case VTERM_PROP_CURSORVISIBLE:
		fprintf(stderr, "cursorvisible %i\n", val->boolean);
		return 0;
	case VTERM_PROP_CURSORBLINK:
		fprintf(stderr, "blink %i\n", val->boolean);
		return 0;
	case VTERM_PROP_MOUSE:
		fprintf(stderr, "mouse %i\n", val->number);
		return 0;
	default:
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

	vterm_color_indexed(&bg, 0);
	vterm_color_indexed(&fg, 7);
#else
	VTermColor bg = {0, 0, 0};
	VTermColor fg = {7, 7, 7};
#endif

	vterm_state_set_default_colors(vs, &fg, &bg);

	vterm_screen_reset(vts, 1);
}

/*
 * Forks and runs a shell, returns master fd.
 */
static int open_console(void)
{
	int fd, pid, flags;

	pid = forkpty(&fd, NULL, NULL, NULL);
	if (pid < 0)
		return -1;

	if (pid == 0) {
		char *shell = getenv("SHELL");

		if (!shell)
			shell = "/bin/sh";

		putenv("TERM=xterm");

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

static int console_read(int fd)
{
	char buf[1024];
	int len;

	len = read(fd, buf, sizeof(buf));

	if (len > 0)
		vterm_input_write(vt, buf, len);

	return len;
}

static void console_write(int fd, char *buf, int buf_len)
{
	write(fd, buf, buf_len);
}

static void console_resize(int fd, int cols, int rows)
{
	struct winsize size = {rows, cols, 0, 0};
	ioctl(fd, TIOCSWINSZ, &size);
}

static void do_exit(int fd)
{
	close_console(fd);
	gp_backend_exit(backend);
	vterm_free(vt);
}

static void key_to_console(gp_event *ev, int fd)
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
	case GP_KEY_DELETE:
		console_write(fd, "\e[3~", 4);
	break;
	case GP_KEY_PAGE_UP:
		console_write(fd, "\e[5~", 4);
	break;
	case GP_KEY_PAGE_DOWN:
		console_write(fd, "\e[6~", 4);
	break;
	case GP_KEY_HOME:
		console_write(fd, "\e[7~", 4);
	break;
	case GP_KEY_END:
		console_write(fd, "\e[8~", 4);
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
};

static void utf_to_console(gp_event *ev, int fd)
{
	char utf_buf[4];
	int bytes = gp_to_utf8(ev->utf.ch, utf_buf);

	write(fd, utf_buf, bytes);
}

struct RGB {
	uint8_t r;
	uint8_t g;
	uint8_t b;
};

struct RGB RGB_colors[16] = {
	/* BLACK */
	{0x00, 0x00, 0x00},
	/* RED */
	{0xcd, 0x00, 0x00},
	/* GREEN */
	{0x00, 0xcd, 0x00},
	/* YELLOW */
	{0xcd, 0xcd, 0x00},
	/* BLUE */
	{0x00, 0x00, 0xee},
	/* MAGENTA */
	{0xcd, 0x00, 0xcd},
	/* CYAN */
	{0x00, 0xcd, 0xcd},
	/* GRAY */
	{0xe5, 0xe5, 0xe5},

	/* BRIGHT BLACK */
	{0x7f, 0x7f, 0x7f},
	/* BRIGHT RED */
	{0xff, 0x00, 0x00},
	/* BRIGHT GREEN */
	{0x00, 0xff, 0x00},
	/* BRIGHT YELLOW */
	{0xff, 0xff, 0x00},
	/* BRIGHT BLUE */
	{0x5c, 0x5c, 0xff},
	/* BRIGHT MAGENTA */
	{0xff, 0x00, 0xff},
	/* BRIGHT CYAN */
	{0x00, 0xff, 0xff},
	/* WHITE */
	{0xff, 0xff, 0xff},
};

static void backend_init(const char *backend_opts)
{
	int i;

	backend = gp_backend_init(backend_opts, "Termini");
	if (!backend) {
		fprintf(stderr, "Failed to initalize backend\n");
		exit(1);
	}

	for (i = 0; i < 16; i++) {
		colors[i] = gp_rgb_to_pixmap_pixel(RGB_colors[i].r,
		                                RGB_colors[i].g,
		                                RGB_colors[i].b,
		                                backend->pixmap);
	}
}

static void print_help(const char *name, int exit_val)
{
	gp_fonts_iter i;
	const gp_font_family *f;

	printf("usage: %s [-b backend_opts] [-f font_family]\n\n", name);

	printf(" -b backend init string (pass -b help for options)\n");
	printf(" -f gfpxrim font family\n");
	printf("    Available fonts families:\n");
	GP_FONT_FAMILY_FOREACH(&i, f)
		printf("\t - %s\n", f->family_name);

	exit(exit_val);
}

int main(int argc, char *argv[])
{
	int opt;
	const char *backend_opts = "x11";
	const char *font_family = "haxor-narrow-18";
	const gp_font_family *ffamily;

	while ((opt = getopt(argc, argv, "b:f:h")) != -1) {
		switch (opt) {
		case 'b':
			backend_opts = optarg;
		break;
		case 'f':
			font_family = optarg;
		break;
		case 'h':
			print_help(argv[0], 0);
		break;
		default:
			print_help(argv[0], 1);
		}
	}

	backend_init(backend_opts);

	ffamily = gp_font_family_lookup(font_family);

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

	cols = gp_pixmap_w(backend->pixmap)/char_width;
	rows = gp_pixmap_h(backend->pixmap)/char_height;

	fprintf(stderr, "Cols %i Rows %i\n", cols, rows);

	term_init();

	int fd = open_console();

	struct pollfd fds[2] = {
		{.fd = fd, .events = POLLIN},
		{.fd = backend->fd, .events = POLLIN}
	};

	console_resize(fd, cols, rows);

	for (;;) {
		gp_event *ev;

		if (poll(fds, 2, repaint_sleep_ms) == 0)
			repaint_damage();

		while ((ev = gp_backend_poll_event(backend))) {
			switch (ev->type) {
			case GP_EV_KEY:
				if (ev->code == GP_EV_KEY_UP)
					break;

				key_to_console(ev, fd);
			break;
			case GP_EV_UTF:
				utf_to_console(ev, fd);
			break;
			case GP_EV_SYS:
				switch (ev->code) {
				case GP_EV_SYS_RESIZE:
					gp_backend_resize_ack(backend);
					cols = ev->sys.w/char_width;
					rows = ev->sys.h/char_height;
					vterm_set_size(vt, rows, cols);
					console_resize(fd, cols, rows);
					gp_fill(backend->pixmap, 0);
					VTermRect rect = {.start_row = 0, .start_col = 0, .end_row = rows, .end_col = cols};
					term_damage(rect, NULL);
					//TODO cursor
				break;
				case GP_EV_SYS_QUIT:
					do_exit(fd);
				break;
				}
			break;
			}
		}

		console_read(fd);
	}

	return 0;
}
