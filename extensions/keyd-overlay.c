#include <X11/Xlib.h>

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WIN_H 40
#define PADDING 40
#define BORDER 1
#define BOTTOM_GAP 60
#define FONT_NAME "-xos4-*-*-*-*-*-*-*-*-*-*-160-*-*"

#define TEXT_SIZE 4096
#define POLL_COUNT 2

static int mapped = 0;

static const char width_reference[] = "12345678";

static int get_window_width(XFontStruct *font, const char *text, int min_width) {
  if (strchr(text, '-') == NULL) return min_width;

  int text_width = XTextWidth(font, text, strlen(text));

  int width = text_width + PADDING + (BORDER * 2);

  return width > min_width ? width : min_width;
}

static unsigned long get_color(Display *dpy, int screen, const char *name) {
  XColor color;
  Colormap cmap = DefaultColormap(dpy, screen);

  if (!XParseColor(dpy, cmap, name, &color) || !XAllocColor(dpy, cmap, &color)) {
    fprintf(stderr, "cannot allocate color: %s\n", name);
    return BlackPixel(dpy, screen);
  }

  return color.pixel;
}


static void draw_overlay(Display *dpy, Window win, GC gc, XFontStruct *font, const char *text, int win_width) {
  int text_len = strlen(text);

  if (text_len == 0) return;

  int text_width = XTextWidth(font, text, text_len);

  XClearWindow(dpy, win);

  int text_x = (win_width - text_width) / 2;
  int text_y = (WIN_H + font->ascent - font->descent) / 2;

  if (text_x < 5) text_x = 5;

  XDrawString(dpy, win, gc, text_x, text_y, text, text_len);
}


static void show_overlay(Display *dpy, Window win, GC gc, XFontStruct *font, const char *text, int win_width) {
  if (text[0] == '\0') return;

  if (!mapped) {
    XMapRaised(dpy, win);
    mapped = 1;
  }

  draw_overlay(dpy, win, gc, font, text, win_width);
  XFlush(dpy);
}


static void hide_overlay(Display *dpy, Window win) {
  if (!mapped) return;

  XUnmapWindow(dpy, win);
  mapped = 0;

  XFlush(dpy);
}


static void
update_overlay(Display *dpy, Window win, GC gc, XFontStruct *font, const char *text, int *win_width, int min_width, int screen_width, int win_y) {
  if (text[0] == '\0') {
    hide_overlay(dpy, win);
    return;
  }

  int new_width = get_window_width(font, text, min_width);

  if (new_width != *win_width) {
    int win_x = (screen_width - new_width) / 2;

    XMoveResizeWindow(dpy, win, win_x, win_y, new_width, WIN_H);

    *win_width = new_width;
  }

  show_overlay(dpy, win, gc, font, text, *win_width);
}


static int process_input(char *input, size_t *input_len, char *text, size_t text_size, const char *buf, ssize_t buf_len) {
  int updated = 0;

  for (ssize_t i = 0; i < buf_len; i++) {
    char c = buf[i];

    if (c == '\r') continue;

    if (c == '\n') {
      input[*input_len] = '\0';

      snprintf(text, text_size, "%s", input);

      *input_len = 0;
      updated = 1;

      continue;
    }

    if (*input_len < text_size - 1) input[(*input_len)++] = c;
  }

  return updated;
}


static int read_stdin(char *input, size_t *input_len, char *text, size_t text_size) {
  char buf[4096];
  ssize_t nread;
  int updated = 0;

  do {
    nread = read(STDIN_FILENO, buf, sizeof(buf));

    if (nread > 0) {
      if (process_input(input, input_len, text, text_size, buf, nread)) {
        updated = 1;
      }
    }

  } while (nread == sizeof(buf));

  if (nread == 0) return -1;

  if (nread < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
    perror("read");
    return -1;
  }

  return updated;
}


static void handle_x_events(Display *dpy, Window win, GC gc, XFontStruct *font, const char *text, int win_width) {
  while (XPending(dpy)) {
    XEvent ev;

    XNextEvent(dpy, &ev);

    if (ev.type == Expose && text[0] != '\0') {
      draw_overlay(dpy, win, gc, font, text, win_width);

      XFlush(dpy);
    }
  }
}


int main(void) {
  Display *dpy = XOpenDisplay(NULL);

  if (!dpy) {
    fprintf(stderr, "cannot open X display\n");
    return 1;
  }

  int screen = DefaultScreen(dpy);
  Window root = RootWindow(dpy, screen);

  /*
   * Font
   */
  XFontStruct *font = XLoadQueryFont(dpy, FONT_NAME);

  if (!font) {
    fprintf(stderr, "cannot load font: %s\n", FONT_NAME);
    XCloseDisplay(dpy);
    return 1;
  }

  /*
   * Colors
   */
  unsigned long bg = get_color(dpy, screen, "#282828");

  unsigned long fg = get_color(dpy, screen, "#ddc7a1");

  unsigned long border = get_color(dpy, screen, "#7c6f64");

  /*
   * Minimum window width.
   *
   * Single modifiers use this width.
   * Longer combinations may expand the window.
   */
  int min_text_width = XTextWidth(font, width_reference, strlen(width_reference));

  int win_width = min_text_width + PADDING + (BORDER * 2);

  int min_width = XTextWidth(font, width_reference, strlen(width_reference)) + PADDING + (BORDER * 2);

  if (win_width < 100) win_width = 100;


  /*
   * Fixed position.
   */
  int screen_width = DisplayWidth(dpy, screen);

  int screen_height = DisplayHeight(dpy, screen);

  int win_x = (screen_width - win_width) / 2;

  int win_y = screen_height - WIN_H - BOTTOM_GAP;

  /*
   * Window.
   */
  XSetWindowAttributes attr;

  attr.override_redirect = True;
  attr.background_pixel = bg;
  attr.border_pixel = border;

  Window win = XCreateWindow(dpy,
                             root,
                             win_x,
                             win_y,
                             win_width,
                             WIN_H,
                             BORDER,
                             CopyFromParent,
                             InputOutput,
                             CopyFromParent,
                             CWOverrideRedirect | CWBackPixel | CWBorderPixel,
                             &attr);

  /*
   * Only Expose is needed.
   */
  XSelectInput(dpy, win, ExposureMask);

  /*
   * GC.
   */
  GC gc = XCreateGC(dpy, win, 0, NULL);

  XSetForeground(dpy, gc, fg);
  XSetFont(dpy, gc, font->fid);

  /*
   * Current displayed text.
   */
  char text[TEXT_SIZE] = "";

  /*
   * Input parser state.
   */
  char input[TEXT_SIZE];
  size_t input_len = 0;

  /*
   * X11 FD.
   */
  int xfd = ConnectionNumber(dpy);

  struct pollfd fds[POLL_COUNT] = {{.fd = STDIN_FILENO, .events = POLLIN}, {.fd = xfd, .events = POLLIN}};

  int running = 1;

  while (running) {
    int ret = poll(fds, POLL_COUNT, -1);

    if (ret < 0) {
      if (errno == EINTR) continue;

      perror("poll");
      break;
    }

    /*
     * ------------------------------------------------
     * STDIN / keyd pipe
     * ------------------------------------------------
     */
    if (fds[0].revents & POLLIN) {
      int result = read_stdin(input, &input_len, text, sizeof(text));

      if (result < 0) {
        running = 0;
        break;
      }

      /*
       * Render only once after all currently
       * available input has been processed.
       */
      if (result > 0) {
        update_overlay(dpy, win, gc, font, text, &win_width, min_width, screen_width, win_y);
      }
    }

    /*
     * ------------------------------------------------
     * X11 events
     * ------------------------------------------------
     */
    if (fds[1].revents & POLLIN) {
      handle_x_events(dpy, win, gc, font, text, win_width);
    }
  }

  XFreeGC(dpy, gc);
  XFreeFont(dpy, font);
  XDestroyWindow(dpy, win);
  XCloseDisplay(dpy);

  return 0;
}
