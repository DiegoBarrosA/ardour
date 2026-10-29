/*
 * demo/ydk_demo_m10.inc
 *
 * M10 exercises: frame pacing, buffer lifecycle, output scale.
 *
 * This is a plain C fragment, not a translation unit.  demo/ydk_demo.c
 * includes it once, before main (), after m9_exercise (), and after
 * <ydk/gdkm10-wayland.h>.  It uses the static helpers and macros of that file
 * (main_window, event_handler, m6_events, m6_clear, m6_drain, m6_run_ms,
 * m6_count, m6_find, m6_recorder, m7_put32, m7_bitmap, m7_rects_equal,
 * m5_fill) and the public hooks of gdkm7, gdkm9 and gdkm10.
 *
 * Entry points
 *   m10_exercise ()       T1 to T22 and T24 on the default weston (scale 1);
 *                         prints "ydk_demo: M10: PASS".  T25 is the rest of
 *                         the demo (M4 to M9) plus scripts/baseline-diff.sh.
 *   m10_real_exercise ()  T23 on a weston started with --scale=2, no hooks
 *                         for the scale itself; prints
 *                         "ydk_demo: M10-REAL2: PASS".
 *
 * Rules that keep the assertions exact (section 5 of the contract header)
 *   - Pacing tests that need a fixed state use the frame and buffer HOLD
 *     modes and fire the callbacks and releases by hook.
 *   - A pump helper dispatches the main loop until a condition holds (500 ms,
 *     10 times that under YDK_DEMO_VALGRIND).
 *   - Tests that assert damage exactness (T1, T7, T11) never call
 *     gdk_cairo_create outside an expose handler, because that is a writer
 *     entry and marks the window unknown_dirty.
 *   - "before the next commit" checks run in frame HOLD mode with the state
 *     WAITING, so no commit can happen by accident.
 *   - Release timing: the noop renderer releases early, pixman one frame
 *     later.  Tests that depend on the exact slot history stash the real
 *     releases (buffer HOLD) and release by hook.
 *
 * Mutation checks (one fix undone at a time in a scratch copy, each must make
 * the demo fail): write to a slot that is not FREE (T6); skip the stale
 * region copy (T7); free the store at resize (T8, valgrind); ignore
 * commit_wanted (T1); commit without waiting (T1); send set_buffer_scale
 * outside the commit (T12); do not scale the device offset (T14); do not
 * divide the cursor hotspot (T19); hide without attach NULL (T4); drop the
 * 0 x 0 configure guard (T9).
 */

/* ---- checks -------------------------------------------------------------- */

static int m10_failures;
static int m10_checks;

#define M10_CHECK(cond, what)						\
  G_STMT_START {							\
    m10_checks++;							\
    if (!(cond))							\
      {									\
	g_printerr ("ydk_demo: M10 check failed: %s\n", what);		\
	m10_failures++;							\
      }									\
  } G_STMT_END

#define M10_EQ_INT(got, want, what)					\
  G_STMT_START {							\
    long g_ = (long) (got), w_ = (long) (want);				\
    m10_checks++;							\
    if (g_ != w_)							\
      {									\
	g_printerr ("ydk_demo: M10 check failed: %s: got %ld want %ld\n", \
		    what, g_, w_);					\
	m10_failures++;							\
      }									\
  } G_STMT_END

#define M10_EQ_HEX(got, want, what)					\
  G_STMT_START {							\
    unsigned long g_ = (unsigned long) (got), w_ = (unsigned long) (want); \
    m10_checks++;							\
    if (g_ != w_)							\
      {									\
	g_printerr ("ydk_demo: M10 check failed: %s: got %08lx want %08lx\n", \
		    what, g_, w_);					\
	m10_failures++;							\
      }									\
  } G_STMT_END

#define M10_HOOK(call, what)						\
  G_STMT_START {							\
    m10_checks++;							\
    if (!(call))							\
      {									\
	g_printerr ("ydk_demo: M10 check failed: %s: hook refused\n", what); \
	m10_failures++;							\
      }									\
  } G_STMT_END

#define M10_RED   0xffff0000u

/* ---- state ---------------------------------------------------------------- */

static GdkDisplay *m10_d;
static GdkWindow  *m10_w;              /* the window of the scale group */
static guint       m10_out2;           /* the fake scale 2 output of the group */
static guint32     m10_time = 1300000;
static gint        m10_paint = -1;     /* 0xRRGGBB filled by the expose handler, -1 none */
static GDKWTestFrameMode  m10_fmode = GDKW_TEST_FRAME_MODE_NORMAL;
static GDKWTestBufferMode m10_bmode = GDKW_TEST_BUFFER_MODE_NORMAL;
static guint m10_size_changed, m10_monitors_changed;

/* ---- event handler: records everything, fills exposes on request --------- */

static void
m10_handler (GdkEvent *event, gpointer data)
{
  if (event->type == GDK_EXPOSE && m10_paint >= 0)
    {
      cairo_t *cr = gdk_cairo_create (event->expose.window);

      gdk_cairo_region (cr, event->expose.region);
      cairo_clip (cr);
      cairo_set_source_rgb (cr, ((m10_paint >> 16) & 0xff) / 255.0,
			    ((m10_paint >> 8) & 0xff) / 255.0,
			    (m10_paint & 0xff) / 255.0);
      cairo_paint (cr);
      cairo_destroy (cr);
    }

  m6_recorder (event, data);
}

/* ---- pump ----------------------------------------------------------------- */

typedef gboolean (*M10Cond) (gpointer data);

static guint
m10_ms (guint ms)
{
  return g_getenv ("YDK_DEMO_VALGRIND") ? ms * 10 : ms;
}

static gboolean
m10_stop_cb (gpointer data)
{
  *(gboolean *) data = TRUE;
  return FALSE;
}

/* dispatches the main loop until @cond holds or @ms (scaled) passed */
static gboolean
m10_pump (M10Cond cond, gpointer data, guint ms)
{
  gboolean timed_out = FALSE;
  guint id;

  if (cond (data))
    return TRUE;
  id = g_timeout_add (m10_ms (ms), m10_stop_cb, &timed_out);
  while (!timed_out && !cond (data))
    g_main_context_iteration (NULL, TRUE);
  if (!timed_out)
    g_source_remove (id);

  return cond (data);
}

static void
m10_run (guint ms)
{
  m6_run_ms (m10_ms (ms));
}

/* dispatches only what is queued for GDK now; does not run idle sources while
 * events are pending */
static void
m10_events (void)
{
  int n = 0;

  while (gdk_events_pending () && n++ < 200)
    g_main_context_iteration (NULL, FALSE);
}

/* ---- hook readers --------------------------------------------------------- */

static gboolean
m10_fi_q (GdkWindow *w, GDKWTestFrameInfo *i)
{
  memset (i, 0, sizeof *i);
  return _gdk_wayland_test_frame_info (w, i);
}

static GDKWTestFrameInfo
m10_fi (GdkWindow *w)
{
  GDKWTestFrameInfo i;

  M10_HOOK (m10_fi_q (w, &i), "frame_info");
  return i;
}

static GDKWTestScaleInfo
m10_si (GdkWindow *w)
{
  GDKWTestScaleInfo i;

  memset (&i, 0, sizeof i);
  M10_HOOK (_gdk_wayland_test_scale_info (w, &i), "scale_info");
  return i;
}

static GDKWTestBackingInfo
m10_bi (GdkWindow *w)
{
  GDKWTestBackingInfo i;

  memset (&i, 0, sizeof i);
  M10_HOOK (_gdk_wayland_test_backing_info (w, &i), "backing_info");
  return i;
}

static GDKWTestBufferInfo
m10_buf (GdkWindow *w, guint index)
{
  GDKWTestBufferInfo i;

  memset (&i, 0, sizeof i);
  i.state = GDKW_TEST_SLOT_NONE;
  _gdk_wayland_test_buffer_info (w, index, &i);
  return i;
}

static GDKWTestBufferCounts
m10_bc (void)
{
  GDKWTestBufferCounts c;

  memset (&c, 0, sizeof c);
  M10_HOOK (_gdk_wayland_test_buffer_counts (m10_d, &c), "buffer_counts");
  return c;
}

static GDKWTestObjectCounts
m10_oc (void)
{
  GDKWTestObjectCounts c;

  memset (&c, 0, sizeof c);
  M10_HOOK (_gdk_wayland_test_object_counts (m10_d, &c), "object_counts");
  return c;
}

static GDKWTestRoleInfo
m10_ri (GdkWindow *w)
{
  GDKWTestRoleInfo i;

  memset (&i, 0, sizeof i);
  M10_HOOK (_gdk_wayland_test_role_info (w, &i), "role_info");
  return i;
}

static GDKWTestPopupInfo
m10_pi (GdkWindow *w)
{
  GDKWTestPopupInfo i;

  memset (&i, 0, sizeof i);
  M10_HOOK (_gdk_wayland_test_popup_info (w, &i), "popup_info");
  return i;
}

/* device pixel (x, y) of the back store described by @bi, 0xAARRGGBB */
static guint32
m10_bpx (const GDKWTestBackingInfo *bi, gint x, gint y)
{
  if (bi->data == NULL || x < 0 || y < 0 || x >= bi->px_width || y >= bi->px_height)
    return 0xdeadbeef;
  return *(const guint32 *) ((const guchar *) bi->data + (gsize) y * bi->stride + 4 * x);
}

static void
m10_reset (GdkWindow *w)
{
  _gdk_wayland_test_frame_reset_counters (w);
}

typedef enum
{
  M10_COMMITS,
  M10_CB_DONE,
  M10_FROM_CB,
  M10_FROM_REL,
  M10_FROM_TO,
  M10_TIMEOUTS,
  M10_STALLED
} M10Counter;

static guint
m10_counter (GdkWindow *w, M10Counter c)
{
  GDKWTestFrameInfo i;

  if (!m10_fi_q (w, &i))
    return 0;
  switch (c)
    {
    case M10_COMMITS:  return i.commits;
    case M10_CB_DONE:  return i.frame_callbacks_done;
    case M10_FROM_CB:  return i.commits_from_callback;
    case M10_FROM_REL: return i.commits_from_release;
    case M10_FROM_TO:  return i.commits_from_timeout;
    case M10_TIMEOUTS: return i.frame_timeouts;
    case M10_STALLED:  return i.stalled_no_buffer;
    }
  return 0;
}

/* ---- wait conditions ------------------------------------------------------ */

typedef struct
{
  GdkWindow *w;
  M10Counter c;
  guint      target;
} M10CounterWait;

static gboolean
m10_counter_reached (gpointer p)
{
  M10CounterWait *cw = p;

  return m10_counter (cw->w, cw->c) >= cw->target;
}

static gboolean
m10_wait_counter (GdkWindow *w, M10Counter c, guint target, const char *what)
{
  M10CounterWait cw;
  gboolean ok;

  cw.w = w;
  cw.c = c;
  cw.target = target;
  ok = m10_pump (m10_counter_reached, &cw, 500);
  M10_CHECK (ok, what);

  return ok;
}

static gboolean
m10_is_idle (gpointer p)
{
  GDKWTestFrameInfo i;

  return m10_fi_q (p, &i) && i.state == GDKW_TEST_FRAME_IDLE && !i.commit_wanted;
}

static gboolean
m10_has_held (gpointer p)
{
  GDKWTestFrameInfo i;

  return m10_fi_q (p, &i) && i.held_done;
}

/* the window is IDLE, nothing wanted, and stays so for a short while */
static void
m10_quiet (GdkWindow *w)
{
  int round;

  for (round = 0; round < 8; round++)
    {
      GDKWTestFrameInfo i;

      m10_pump (m10_is_idle, w, 800);
      m10_run (40);
      if (m10_fi_q (w, &i) && i.state == GDKW_TEST_FRAME_IDLE &&
	  !i.commit_wanted && !i.callback_outstanding)
	return;
    }
}

static void
m10_set_fmode (GDKWTestFrameMode mode)
{
  m10_fmode = mode;
  M10_HOOK (_gdk_wayland_test_frame_mode (m10_d, mode), "frame mode");
}

static void
m10_set_bmode (GDKWTestBufferMode mode)
{
  m10_bmode = mode;
  M10_HOOK (_gdk_wayland_test_buffer_mode (m10_d, mode), "buffer mode");
}

/* acts on the outstanding frame callback of @w, in HOLD mode with the stashed
 * real event, else through the fire hook */
static void
m10_fire (GdkWindow *w)
{
  GDKWTestFrameInfo i;

  if (!m10_fi_q (w, &i))
    return;
  if (m10_fmode == GDKW_TEST_FRAME_MODE_HOLD && !i.held_done && i.callback_outstanding)
    m10_pump (m10_has_held, w, 150);
  if (!m10_fi_q (w, &i))
    return;
  if (i.held_done)
    _gdk_wayland_test_frame_release_held (w);
  else if (i.callback_outstanding)
    _gdk_wayland_test_frame_fire (w);
}

static void
m10_fire_if_waiting (GdkWindow *w)
{
  GDKWTestFrameInfo i;

  if (m10_fi_q (w, &i) && (i.callback_outstanding || i.held_done))
    m10_fire (w);
}

/* ---- windows and drawing -------------------------------------------------- */

static GdkWindow *
m10_new_ex (gint width, gint height, gboolean settle)
{
  GdkWindowAttr attr;
  GdkWindow *win;

  memset (&attr, 0, sizeof attr);
  attr.window_type = GDK_WINDOW_TOPLEVEL;
  attr.wclass = GDK_INPUT_OUTPUT;
  attr.width = width;
  attr.height = height;
  attr.event_mask = GDK_STRUCTURE_MASK | GDK_EXPOSURE_MASK |
    GDK_POINTER_MOTION_MASK | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK;
  win = gdk_window_new (gdk_get_default_root_window (), &attr, 0);
  M10_CHECK (win != NULL, "test window created");
  if (win == NULL)
    return NULL;
  gdk_window_show (win);
  m10_wait_counter (win, M10_COMMITS, 1, "new window: first commit");
  if (settle)
    {
      m10_wait_counter (win, M10_CB_DONE, 1, "new window: first frame callback");
      m10_quiet (win);
      m10_reset (win);
    }
  m6_clear ();

  return win;
}

/* shown, configured, first frame done, quiet, counters zero */
static GdkWindow *
m10_new (gint width, gint height)
{
  return m10_new_ex (width, height, TRUE);
}

static void
m10_destroy (GdkWindow *w)
{
  if (w != NULL)
    gdk_window_destroy (w);
  m6_drain ();
}

/* invalidate a rect and process the update: the expose handler fills it */
static void
m10_redraw (GdkWindow *w, gint x, gint y, gint rw, gint rh, gint rgb)
{
  GdkRectangle r = { x, y, rw, rh };

  m10_paint = rgb;
  gdk_window_invalidate_rect (w, &r, FALSE);
  gdk_window_process_updates (w, TRUE);
}

/* a commit through the real present path; waits for it */
static void
m10_commit_now (GdkWindow *w)
{
  guint base = m10_counter (w, M10_COMMITS);

  _gdk_wayland_test_frame_present (w);
  m10_wait_counter (w, M10_COMMITS, base + 1, "present: commit made");
}

static void
m10_release_superseded (GdkWindow *w)
{
  guint n = _gdk_wayland_test_buffer_count (w), i;

  for (i = 0; i < n; i++)
    {
      GDKWTestBufferInfo b = m10_buf (w, i);

      if (b.state == GDKW_TEST_SLOT_SUPERSEDED)
	_gdk_wayland_test_buffer_release (w, i);
    }
}

/* HOLD mode: fire until the window has nothing wanted */
static void
m10_settle_hold (GdkWindow *w)
{
  int i;

  for (i = 0; i < 6; i++)
    {
      GDKWTestFrameInfo fi;

      m10_run (40);
      if (!m10_fi_q (w, &fi))
	return;
      if (!fi.commit_wanted && !fi.held_done && !fi.callback_outstanding)
	return;
      m10_fire (w);
    }
}

static gboolean
m10_rect_in (const GdkRectangle *outer, const GdkRectangle *inner)
{
  return inner->x >= outer->x && inner->y >= outer->y &&
    inner->x + inner->width <= outer->x + outer->width &&
    inner->y + inner->height <= outer->y + outer->height;
}

static int
m10_pos (GdkEventType type, GdkWindow *w)
{
  GList *l;
  int i = 0;

  for (l = m6_events.head; l; l = l->next, i++)
    {
      GdkEvent *ev = l->data;

      if (ev->type == type && (w == NULL || ev->any.window == w))
	return i;
    }

  return -1;
}

/* ---- T1 pacing and coalescing -------------------------------------------- */

static void
m10_t1 (void)
{
  static const GdkRectangle four[4] = {
    { 0, 0, 10, 10 }, { 50, 50, 10, 10 }, { 100, 20, 10, 10 }, { 0, 100, 10, 10 }
  };
  GdkWindow *w = m10_new (200, 120);
  GDKWTestFrameInfo fi;
  int i, k;

  if (w == NULL)
    return;
  fi = m10_fi (w);
  M10_EQ_INT (fi.state, GDKW_TEST_FRAME_IDLE, "T1: IDLE after the first frame done");

  m10_redraw (w, 150, 80, 10, 10, 0x2040a0);
  fi = m10_fi (w);
  M10_EQ_INT (fi.commits, 1, "T1: first request commits");
  M10_EQ_INT (fi.commits_immediate, 1, "T1: first commit is immediate");
  M10_EQ_INT (fi.frame_requests, 1, "T1: one frame request");
  M10_EQ_INT (fi.state, GDKW_TEST_FRAME_WAITING, "T1: WAITING");

  for (i = 0; i < 4; i++)
    m10_redraw (w, four[i].x, four[i].y, four[i].width, four[i].height,
		0x30 + 0x20 * i);
  fi = m10_fi (w);
  M10_EQ_INT (fi.commits, 1, "T1: still one commit");
  M10_EQ_INT (fi.commit_requests, 5, "T1: five commit requests");
  M10_EQ_INT (fi.coalesced, 4, "T1: four coalesced");
  M10_CHECK (fi.commit_wanted, "T1: commit_wanted");
  M10_EQ_INT (fi.frame_requests, 1, "T1: still one frame request");

  m10_wait_counter (w, M10_CB_DONE, 1, "T1: frame callback done");
  fi = m10_fi (w);
  M10_EQ_INT (fi.commits, 2, "T1: second commit at the callback");
  M10_EQ_INT (fi.commits_from_callback, 1, "T1: it came from the callback");
  M10_CHECK (!fi.commit_wanted, "T1: commit_wanted cleared");
  M10_EQ_INT (fi.frame_requests, 2, "T1: second frame request");
  M10_EQ_INT (fi.last_damage_area, 400, "T1: damage area 400");
  M10_CHECK (fi.last_damage_n >= 1 && fi.last_damage_n <= GDKW_TEST_MAX_DAMAGE,
	     "T1: damage rect count");
  for (i = 0; i < 4; i++)
    {
      gboolean inside = FALSE;

      for (k = 0; k < fi.last_damage_n && k < GDKW_TEST_MAX_DAMAGE; k++)
	if (m10_rect_in (&fi.last_damage[k], &four[i]))
	  inside = TRUE;
      M10_CHECK (inside, "T1: each rect is inside the damage");
    }

  m10_paint = -1;
  m10_destroy (w);
}

/* ---- T2 withheld and dropped callbacks ------------------------------------ */

static gboolean
m10_redraw_tick (gpointer data)
{
  m10_redraw (data, 0, 0, 8, 8, 0x808080);
  return TRUE;
}

static gboolean
m10_timeouts_reset (gpointer p)
{
  GDKWTestFrameInfo i;

  return m10_fi_q (p, &i) && i.consecutive_timeouts == 0 && i.timeout_ms == 30;
}

static void
m10_t2 (void)
{
  GdkWindow *w = m10_new (200, 120);
  GDKWTestFrameInfo fi;
  guint tick;
  int i;

  if (w == NULL)
    return;

  m10_set_fmode (GDKW_TEST_FRAME_MODE_HOLD);
  m10_reset (w);
  for (i = 0; i < 10; i++)
    m10_redraw (w, 10 * i, 5, 8, 8, 0x404040);
  fi = m10_fi (w);
  M10_EQ_INT (fi.commits, 1, "T2: HOLD: first request commits");
  m10_run (100);
  fi = m10_fi (w);
  M10_EQ_INT (fi.commits, 1, "T2: HOLD: no commit after 100 ms");
  M10_CHECK (fi.held_done, "T2: HOLD: the real callback was stashed");
  M10_EQ_INT (fi.frame_timeouts, 0, "T2: HOLD: no timeout");
  M10_HOOK (_gdk_wayland_test_frame_release_held (w), "T2: release_held");
  fi = m10_fi (w);
  M10_EQ_INT (fi.commits, 2, "T2: release_held commits");

  /* back to NORMAL, wait for IDLE, then drop callbacks */
  m10_set_fmode (GDKW_TEST_FRAME_MODE_NORMAL);
  m10_quiet (w);
  M10_HOOK (_gdk_wayland_test_frame_timeouts (m10_d, 30, 60), "T2: timeouts");
  m10_set_fmode (GDKW_TEST_FRAME_MODE_DROP);
  m10_reset (w);
  m10_redraw (w, 0, 0, 8, 8, 0x808080);
  tick = g_timeout_add (10, m10_redraw_tick, w);
  m10_run (300);
  g_source_remove (tick);
  fi = m10_fi (w);
  M10_CHECK (fi.frame_timeouts >= 3, "T2: DROP: at least 3 timeouts");
  M10_CHECK (fi.commits_from_timeout >= 1, "T2: DROP: commit from a timeout");
  M10_EQ_INT (fi.timeout_ms, 60, "T2: DROP: backoff capped at 60");
  M10_CHECK (fi.consecutive_timeouts >= 3, "T2: DROP: consecutive timeouts");

  m10_set_fmode (GDKW_TEST_FRAME_MODE_NORMAL);
  m10_redraw (w, 0, 0, 8, 8, 0x808080);
  M10_CHECK (m10_pump (m10_timeouts_reset, w, 1000),
	     "T2: a real callback resets the backoff to 30");
  M10_HOOK (_gdk_wayland_test_frame_timeouts (m10_d, 0, 0), "T2: restore timeouts");

  m10_paint = -1;
  m10_destroy (w);
}

/* ---- T3 immediate commit -------------------------------------------------- */

static void
m10_t3 (void)
{
  GdkWindow *w = m10_new (200, 120);
  GDKWTestFrameInfo fi;

  if (w == NULL)
    return;
  m10_redraw (w, 0, 0, 10, 10, 0x102030);	/* WAITING with a callback */
  fi = m10_fi (w);
  M10_CHECK (fi.callback_outstanding, "T3: a callback is outstanding");
  M10_HOOK (_gdk_wayland_test_frame_fire (w), "T3: fire");
  fi = m10_fi (w);
  M10_EQ_INT (fi.state, GDKW_TEST_FRAME_IDLE, "T3: IDLE after the fire");
  m10_reset (w);
  m10_redraw (w, 20, 20, 10, 10, 0x102030);
  fi = m10_fi (w);
  M10_EQ_INT (fi.commits, 1, "T3: the request commits in the same call");
  M10_EQ_INT (fi.commits_immediate, 1, "T3: immediate");

  m10_paint = -1;
  m10_destroy (w);
}

/* ---- T4 hidden window ------------------------------------------------------ */

static void
m10_t4 (void)
{
  GdkWindow *w = m10_new (200, 120);
  GDKWTestFrameInfo fi;
  GDKWTestObjectCounts oc0, oc1;
  GDKWTestRoleInfo ri;
  guint commits0;

  if (w == NULL)
    return;
  m10_redraw (w, 0, 0, 10, 10, 0x102030);
  m10_redraw (w, 20, 20, 10, 10, 0x102030);
  fi = m10_fi (w);
  M10_EQ_INT (fi.state, GDKW_TEST_FRAME_WAITING, "T4: WAITING before the hide");
  M10_CHECK (fi.commit_wanted, "T4: commit_wanted before the hide");
  oc0 = m10_oc ();
  m10_reset (w);

  gdk_window_hide (w);
  fi = m10_fi (w);
  M10_EQ_INT (fi.state, GDKW_TEST_FRAME_UNMAPPED, "T4: UNMAPPED");
  M10_CHECK (!fi.callback_outstanding, "T4: callback destroyed");
  M10_CHECK (!fi.timeout_armed, "T4: timeout disarmed");
  M10_CHECK (!fi.settle_armed, "T4: settle idle disarmed");
  M10_CHECK (!fi.commit_wanted, "T4: commit_wanted cleared");
  M10_EQ_INT (fi.attach_null, 1, "T4: one attach NULL");
  gdk_flush ();
  m6_drain ();
  oc1 = m10_oc ();
  M10_EQ_INT (oc1.xdg_toplevels, (long) oc0.xdg_toplevels - 1, "T4: xdg_toplevel destroyed");
  M10_EQ_INT (oc1.xdg_surfaces, (long) oc0.xdg_surfaces - 1, "T4: xdg_surface destroyed");
  ri = m10_ri (w);
  M10_CHECK (!ri.configured && !ri.has_xdg_toplevel, "T4: role gone");
  M10_CHECK (ri.has_wl_surface, "T4: the wl_surface stays");

  commits0 = m10_fi (w).commits;
  _gdk_wayland_test_frame_present (w);
  fi = m10_fi (w);
  M10_EQ_INT (fi.commits, commits0, "T4: a present request while hidden commits nothing");

  m10_reset (w);
  gdk_window_show (w);
  m10_wait_counter (w, M10_COMMITS, 1, "T4: commit after the show");
  fi = m10_fi (w);
  M10_CHECK (fi.last_commit_full, "T4: the first commit after the show is FULL");
  ri = m10_ri (w);
  M10_CHECK (ri.configured, "T4: configured again (a new configure was acked)");
  oc1 = m10_oc ();
  M10_EQ_INT (oc1.xdg_toplevels, oc0.xdg_toplevels, "T4: xdg_toplevel back");
  M10_EQ_INT (oc1.xdg_surfaces, oc0.xdg_surfaces, "T4: xdg_surface back");

  m10_paint = -1;
  m10_destroy (w);
}

/* ---- T5 release frees a buffer for reuse ----------------------------------- */

static gboolean
m10_any_released (gpointer p)
{
  guint n = _gdk_wayland_test_buffer_count (p), i;

  for (i = 0; i < n; i++)
    if (m10_buf (p, i).released_count >= 1)
      return TRUE;

  return FALSE;
}

static void
m10_t5 (void)
{
  static const int colors[6] = { 0xff0000, 0x00ff00, 0x0000ff, 0xffff00, 0x00ffff, 0xff00ff };
  GDKWTestBufferCounts bc0 = m10_bc (), bc1;
  GdkWindow *w = m10_new (200, 120);
  gboolean twice = FALSE;
  guint n, i;

  if (w == NULL)
    return;
  for (i = 0; i < 6; i++)
    {
      m10_redraw (w, 0, 0, 200, 120, colors[i]);
      m10_wait_counter (w, M10_CB_DONE, i + 1, "T5: paced frame callback");
    }
  M10_CHECK (m10_pump (m10_any_released, w, 500), "T5: a buffer was released");
  bc1 = m10_bc ();
  M10_CHECK (bc1.buffers_created - bc0.buffers_created <= 3, "T5: at most 3 buffers created");
  n = _gdk_wayland_test_buffer_count (w);
  for (i = 0; i < n; i++)
    if (m10_buf (w, i).times_attached >= 2)
      twice = TRUE;
  M10_CHECK (twice, "T5: a slot was attached twice");

  m10_paint = -1;
  m10_destroy (w);
}

/* ---- T6 held release and poison -------------------------------------------- */

static void
m10_t6 (guint limit)
{
  static const int colors[8] = {
    0xff0000, 0x0000ff, 0x00ff00, 0xffffff, 0x808080, 0xff00ff, 0x00ffff, 0x404040
  };
  GdkWindow *w;
  GDKWTestFrameInfo fi;
  GDKWTestBufferInfo b;
  guint64 sum0 = 0, sum1 = 0;
  guint32 px = 0;
  guint mism = 99;
  int i;
  char msg[96];

  m10_set_fmode (GDKW_TEST_FRAME_MODE_HOLD);
  m10_set_bmode (GDKW_TEST_BUFFER_MODE_HOLD);
  if (limit != 0)
    M10_HOOK (_gdk_wayland_test_buffer_limit (m10_d, limit), "T6: buffer_limit");

  w = m10_new_ex (200, 120, FALSE);
  if (w != NULL)
    {
      m10_settle_hold (w);
      m10_reset (w);
      M10_HOOK (_gdk_wayland_test_buffer_poison (w, 0, 0xdeadbeef), "T6: poison slot 0");
      M10_HOOK (_gdk_wayland_test_buffer_checksum (w, 0, &sum0), "T6: checksum slot 0");

      for (i = 0; i < 8; i++)
	{
	  m10_fire_if_waiting (w);
	  m10_redraw (w, 0, 0, 200, 120, colors[i]);
	  if (m10_counter (w, M10_STALLED) > 0)
	    break;
	}
      fi = m10_fi (w);
      g_snprintf (msg, sizeof msg, "T6 (limit %u): the commit stalls", limit);
      M10_EQ_INT (fi.stalled_no_buffer, 1, msg);
      M10_CHECK (fi.commit_wanted, "T6: the stalled commit stays wanted");
      g_snprintf (msg, sizeof msg, "T6 (limit %u): slot count", limit);
      M10_EQ_INT (_gdk_wayland_test_buffer_count (w), limit != 0 ? limit : 4, msg);
      b = m10_buf (w, 0);
      M10_CHECK (b.state == GDKW_TEST_SLOT_SUPERSEDED || b.state == GDKW_TEST_SLOT_ATTACHED,
		 "T6: slot 0 is still held by the compositor");
      M10_CHECK (b.poisoned, "T6: slot 0 is marked poisoned");
      M10_HOOK (_gdk_wayland_test_buffer_checksum (w, 0, &sum1), "T6: checksum again");
      M10_CHECK (sum0 == sum1, "T6: slot 0 was not written while held");

      m10_reset (w);
      M10_HOOK (_gdk_wayland_test_buffer_release (w, 0), "T6: release slot 0");
      fi = m10_fi (w);
      M10_EQ_INT (fi.commits_from_release, 1, "T6: the release resumes the commit");
      b = m10_buf (w, 0);
      M10_EQ_INT (b.state, GDKW_TEST_SLOT_ATTACHED, "T6: slot 0 is now ATTACHED");
      M10_HOOK (_gdk_wayland_test_buffer_vs_backing (w, 0, &mism), "T6: vs_backing");
      M10_EQ_INT (mism, 0, "T6: slot 0 equals the store");
      M10_HOOK (_gdk_wayland_test_buffer_pixel (w, 0, 0, 0, &px), "T6: pixel");
      M10_CHECK (px != 0xdeadbeef, "T6: the poison is gone");
    }

  m10_paint = -1;
  m10_set_fmode (GDKW_TEST_FRAME_MODE_NORMAL);
  m10_set_bmode (GDKW_TEST_BUFFER_MODE_NORMAL);
  M10_HOOK (_gdk_wayland_test_buffer_limit (m10_d, 0), "T6: restore limit");
  m10_destroy (w);
}

/* ---- T7 stale region exactness --------------------------------------------- */

static void
m10_t7_check (GdkWindow *w, guint commits0, gsize bytes, gint area, const char *what)
{
  GDKWTestFrameInfo fi = m10_fi (w);
  guint mism = 99;
  char msg[128];

  g_snprintf (msg, sizeof msg, "T7: %s: a commit was made", what);
  M10_EQ_INT (fi.commits, commits0 + 1, msg);
  g_snprintf (msg, sizeof msg, "T7: %s: bytes copied", what);
  M10_EQ_INT (fi.last_bytes_copied, bytes, msg);
  g_snprintf (msg, sizeof msg, "T7: %s: damage area", what);
  M10_EQ_INT (fi.last_damage_area, area, msg);
  M10_HOOK (_gdk_wayland_test_buffer_vs_backing (w, fi.last_slot, &mism), "T7: vs_backing");
  g_snprintf (msg, sizeof msg, "T7: %s: slot equals the store", what);
  M10_EQ_INT (mism, 0, msg);
}

/* one commit with the lag of exactly one commit for a two slot pool */
static void
m10_t7_step (GdkWindow *w, gint x, gint y, gint rw, gint rh, gint rgb)
{
  m10_release_superseded (w);
  m10_fire_if_waiting (w);
  m10_redraw (w, x, y, rw, rh, rgb);
}

static void
m10_t7 (void)
{
  GdkWindow *w;
  GDKWTestFrameInfo fi;
  guint mism = 99;
  guint commits0;
  int i;

  m10_set_fmode (GDKW_TEST_FRAME_MODE_HOLD);
  m10_set_bmode (GDKW_TEST_BUFFER_MODE_HOLD);
  M10_HOOK (_gdk_wayland_test_buffer_limit (m10_d, 2), "T7: buffer_limit 2");

  w = m10_new_ex (200, 120, FALSE);
  if (w != NULL)
    {
      fi = m10_fi (w);
      M10_EQ_INT (fi.commits, 1, "T7: first commit");
      M10_EQ_INT (fi.last_slot, 0, "T7: first commit uses slot 0");
      M10_EQ_INT (fi.last_bytes_copied, 4 * 200 * 120, "T7: a new slot copies the whole buffer");
      M10_HOOK (_gdk_wayland_test_buffer_vs_backing (w, 0, &mism), "T7: vs_backing");
      M10_EQ_INT (mism, 0, "T7: first commit equals the store");

      m10_settle_hold (w);
      for (i = 0; i < 3; i++)
	m10_t7_step (w, 0, 0, 10, 10, 0x336699);

      commits0 = m10_fi (w).commits;
      m10_t7_step (w, 10, 10, 30, 20, 0x993366);
      m10_t7_check (w, commits0, 4 * (10 * 10 + 30 * 20), 30 * 20, "R1");

      commits0 = m10_fi (w).commits;
      m10_t7_step (w, 100, 50, 40, 40, 0x669933);
      m10_t7_check (w, commits0, 4 * (30 * 20 + 40 * 40), 40 * 40, "R2");
    }

  m10_paint = -1;
  m10_set_fmode (GDKW_TEST_FRAME_MODE_NORMAL);
  m10_set_bmode (GDKW_TEST_BUFFER_MODE_NORMAL);
  M10_HOOK (_gdk_wayland_test_buffer_limit (m10_d, 0), "T7: restore limit");
  m10_destroy (w);
}

/* ---- T8 resize keeps old surfaces valid ------------------------------------ */

static void
m10_t8 (void)
{
  GdkWindow *w = m10_new (200, 120);
  GDKWTestBackingInfo b0, b1, b2;
  GDKWTestFrameInfo fi;
  cairo_surface_t *s_old, *s_new;
  cairo_t *cr;
  GdkEvent *ev;

  if (w == NULL)
    return;
  cr = gdk_cairo_create (w);
  s_old = cairo_surface_reference (cairo_get_target (cr));
  cairo_destroy (cr);
  b0 = m10_bi (w);
  M10_CHECK (b0.live_surfaces >= 2, "T8: the store has at least two surfaces");
  M10_EQ_INT (b0.px_width, 200, "T8: old store width");

  m10_reset (w);
  m6_clear ();
  M10_HOOK (_gdk_wayland_test_toplevel_configure (w, 300, 200, 0), "T8: configure");
  m10_events ();
  M10_EQ_INT (gdk_window_get_width (w), 300, "T8: width follows the configure");
  M10_EQ_INT (gdk_window_get_height (w), 200, "T8: height follows the configure");
  M10_EQ_INT (m6_count (GDK_CONFIGURE, w), 1, "T8: exactly one GDK_CONFIGURE");
  ev = m6_find (GDK_CONFIGURE, w);
  if (ev != NULL)
    {
      M10_EQ_INT (ev->configure.width, 300, "T8: event width");
      M10_EQ_INT (ev->configure.height, 200, "T8: event height");
    }
  fi = m10_fi (w);
  M10_EQ_INT (fi.commits, 0, "T8: no commit before the settle idle ran");

  b1 = m10_bi (w);
  M10_CHECK (b1.data != b0.data, "T8: a new store");
  M10_EQ_INT (b1.px_width, 300, "T8: new store width");
  M10_EQ_INT (b1.px_height, 200, "T8: new store height");
  M10_EQ_INT (b1.size_mismatches, 0, "T8: no size mismatch");
  M10_EQ_INT (b1.stores_freed, b0.stores_freed, "T8: the old store is still alive");

  /* draw on the old surface: valgrind must stay quiet, the new store is
   * untouched */
  cr = cairo_create (s_old);
  cairo_set_source_rgb (cr, 0.0, 1.0, 0.0);
  cairo_rectangle (cr, 10, 10, 50, 50);
  cairo_fill (cr);
  cairo_destroy (cr);
  cairo_surface_flush (s_old);
  b2 = m10_bi (w);
  M10_CHECK (m10_bpx (&b2, 20, 20) != 0xff00ff00u, "T8: the new store is not green");
  M10_EQ_INT (b2.stores_freed, b0.stores_freed, "T8: still alive after drawing");

  cairo_surface_destroy (s_old);
  b2 = m10_bi (w);
  M10_EQ_INT (b2.stores_freed, b0.stores_freed + 1, "T8: the old store is freed with its last surface");

  cr = gdk_cairo_create (w);
  s_new = cairo_get_target (cr);
  M10_EQ_INT (cairo_image_surface_get_width (s_new), 300, "T8: new surface width");
  M10_EQ_INT (cairo_image_surface_get_height (s_new), 200, "T8: new surface height");
  cairo_destroy (cr);

  m10_wait_counter (w, M10_COMMITS, 1, "T8: a commit of the new size");
  fi = m10_fi (w);
  M10_EQ_INT (fi.last_attach_width, 300, "T8: attach width");
  M10_EQ_INT (fi.last_attach_height, 200, "T8: attach height");
  M10_CHECK (fi.last_commit_full, "T8: the commit is FULL");

  m10_paint = -1;
  m10_destroy (w);
}

/* ---- T9 maximize gap ---------------------------------------------------------- */

static void
m10_t9 (void)
{
  GdkWindow *w = m10_new (200, 120);
  GdkRegion *ua;
  GdkRectangle cb;
  int ic, is;

  if (w == NULL)
    return;
  m6_clear ();
  M10_HOOK (_gdk_wayland_test_toplevel_configure (w, 400, 300, GDKW_TEST_STATE_MAXIMIZED),
	    "T9: maximize configure");
  m10_events ();
  ic = m10_pos (GDK_CONFIGURE, w);
  is = m10_pos (GDK_WINDOW_STATE, w);
  M10_CHECK (ic >= 0, "T9: a GDK_CONFIGURE was queued");
  M10_CHECK (is >= 0, "T9: a GDK_WINDOW_STATE was queued");
  M10_CHECK (ic >= 0 && is >= 0 && ic < is, "T9: GDK_CONFIGURE comes before the state event");
  M10_EQ_INT (m6_count (GDK_CONFIGURE, w), 1, "T9: one GDK_CONFIGURE");
  M10_EQ_INT (gdk_window_get_width (w), 400, "T9: width 400");
  M10_EQ_INT (gdk_window_get_height (w), 300, "T9: height 300");
  ua = gdk_window_get_update_area (w);
  M10_CHECK (ua != NULL, "T9: the window is invalidated");
  if (ua != NULL)
    {
      gdk_region_get_clipbox (ua, &cb);
      M10_CHECK (cb.x == 0 && cb.y == 0 && cb.width == 400 && cb.height == 300,
		 "T9: invalidated in full");
      gdk_region_destroy (ua);
    }

  m6_clear ();
  M10_HOOK (_gdk_wayland_test_toplevel_configure (w, 400, 300, GDKW_TEST_STATE_ACTIVATED),
	    "T9: same size configure");
  m10_events ();
  M10_EQ_INT (m6_count (GDK_CONFIGURE, w), 0, "T9: same size queues no GDK_CONFIGURE");

  m6_clear ();
  M10_HOOK (_gdk_wayland_test_toplevel_configure (w, 0, 0, GDKW_TEST_STATE_ACTIVATED),
	    "T9: 0 x 0 configure");
  m10_events ();
  M10_EQ_INT (m6_count (GDK_CONFIGURE, w), 0, "T9: 0 x 0 queues no GDK_CONFIGURE");
  M10_EQ_INT (gdk_window_get_width (w), 400, "T9: 0 x 0 keeps the width");
  M10_EQ_INT (gdk_window_get_height (w), 300, "T9: 0 x 0 keeps the height");

  m10_paint = -1;
  m10_destroy (w);
}

/* ---- T10 client resize ---------------------------------------------------------- */

static void
m10_t10 (void)
{
  GdkWindow *w = m10_new (200, 120);
  GDKWTestBackingInfo bi;
  cairo_surface_t *s_old;
  cairo_t *cr;
  GdkEvent *ev;

  if (w == NULL)
    return;
  cr = gdk_cairo_create (w);
  s_old = cairo_surface_reference (cairo_get_target (cr));
  cairo_destroy (cr);

  m6_clear ();
  gdk_window_resize (w, 250, 150);
  m10_events ();
  M10_EQ_INT (m6_count (GDK_CONFIGURE, w), 1, "T10: one GDK_CONFIGURE");
  ev = m6_find (GDK_CONFIGURE, w);
  if (ev != NULL)
    {
      M10_EQ_INT (ev->configure.width, 250, "T10: event width");
      M10_EQ_INT (ev->configure.height, 150, "T10: event height");
    }
  bi = m10_bi (w);
  M10_EQ_INT (bi.px_width, 250, "T10: backing width");
  M10_EQ_INT (bi.px_height, 150, "T10: backing height");

  m6_clear ();
  gdk_window_resize (w, 250, 150);
  m10_events ();
  M10_EQ_INT (m6_count (GDK_CONFIGURE, w), 0, "T10: the same size gives no event");

  cr = cairo_create (s_old);
  cairo_set_source_rgb (cr, 1.0, 0.0, 0.0);
  cairo_rectangle (cr, 0, 0, 100, 100);
  cairo_fill (cr);
  cairo_destroy (cr);
  cairo_surface_destroy (s_old);

  m10_paint = -1;
  m10_destroy (w);
}

/* ---- T11 damage exactness -------------------------------------------------------- */

static void
m10_t11 (void)
{
  GdkWindow *w = m10_new (200, 120);
  GDKWTestFrameInfo fi;
  GdkGC *gc;
  guint base;

  if (w == NULL)
    return;
  m10_reset (w);
  m10_redraw (w, 30, 20, 40, 10, 0x55aa55);
  fi = m10_fi (w);
  M10_EQ_INT (fi.commits, 1, "T11: one commit");
  M10_EQ_INT (fi.last_damage_n, 1, "T11: one damage rect");
  M10_CHECK (fi.last_damage[0].x == 30 && fi.last_damage[0].y == 20 &&
	     fi.last_damage[0].width == 40 && fi.last_damage[0].height == 10,
	     "T11: the damage is exactly the expose rect");
  M10_CHECK (!fi.last_commit_full, "T11: not FULL");

  m10_quiet (w);
  base = m10_fi (w).commits;
  gdk_flush ();
  m10_wait_counter (w, M10_COMMITS, base + 1, "T11: gdk_flush commits");
  fi = m10_fi (w);
  M10_CHECK (fi.last_commit_full, "T11: gdk_flush outside a cycle gives FULL");

  m10_quiet (w);
  gc = gdk_gc_new (w);
  m5_fill (w, gc, 0x3366cc, 5, 5, 20, 20);
  fi = m10_fi (w);
  M10_CHECK (fi.unknown_dirty, "T11: a direct draw marks the window unknown_dirty");
  base = fi.commits;
  gdk_display_flush (m10_d);
  m10_wait_counter (w, M10_COMMITS, base + 1, "T11: flush after a direct draw commits");
  fi = m10_fi (w);
  M10_CHECK (fi.last_commit_full, "T11: the commit after unknown_dirty is FULL");
  M10_CHECK (!fi.unknown_dirty, "T11: unknown_dirty cleared");
  g_object_unref (gc);

  m10_paint = -1;
  m10_destroy (w);
}

/* ---- scale group helpers ---------------------------------------------------------- */

static guint
m10_fake_out (gint x, gint y, gint mw, gint mh, gint scale, gint transform)
{
  guint id = 0;

  M10_HOOK (_gdk_wayland_test_output_add (m10_d, x, y, mw, mh, scale, transform, &id),
	    "output_add");
  return id;
}

static guint
m10_real_id (void)
{
  guint n = _gdk_wayland_test_output_count (m10_d), i;

  for (i = 0; i < n; i++)
    {
      GDKWTestOutputInfo oi;

      memset (&oi, 0, sizeof oi);
      if (_gdk_wayland_test_output_info (m10_d, i, &oi) && !oi.fake)
	return oi.id;
    }

  return 0;
}

static gboolean
m10_is_entered (GdkWindow *w, guint id)
{
  GDKWTestScaleInfo s;
  guint i;

  memset (&s, 0, sizeof s);
  if (!_gdk_wayland_test_scale_info (w, &s))
    return FALSE;
  for (i = 0; i < s.n_entered && i < GDKW_TEST_MAX_ENTERED; i++)
    if (s.entered_ids[i] == id)
      return TRUE;

  return FALSE;
}

static void
m10_enter (GdkWindow *w, guint id)
{
  if (id != 0 && !m10_is_entered (w, id))
    M10_HOOK (_gdk_wayland_test_surface_enter (w, id), "surface_enter");
}

static void
m10_leave (GdkWindow *w, guint id)
{
  if (id != 0 && m10_is_entered (w, id))
    M10_HOOK (_gdk_wayland_test_surface_leave (w, id), "surface_leave");
}

static gboolean
m10_entered_any (gpointer p)
{
  GDKWTestScaleInfo s;

  memset (&s, 0, sizeof s);
  return _gdk_wayland_test_scale_info (p, &s) && s.n_entered >= 1;
}

static gboolean
m10_no_slots_of (GdkWindow *w, gint width, gint height)
{
  guint n = _gdk_wayland_test_buffer_count (w), i;

  for (i = 0; i < n; i++)
    {
      GDKWTestBufferInfo b = m10_buf (w, i);

      if (b.state != GDKW_TEST_SLOT_NONE && b.width == width && b.height == height)
	return FALSE;
    }

  return TRUE;
}

typedef struct
{
  GdkWindow *w;
  gint width, height;
} M10SlotWait;

static gboolean
m10_no_slots_cond (gpointer p)
{
  M10SlotWait *sw = p;

  return m10_no_slots_of (sw->w, sw->width, sw->height);
}

typedef struct
{
  gdouble ex, ey, mx, my, mxr, myr;
  gint px, py;
  gboolean got_enter, got_motion;
} M10Ptr;

/* the same injected sequence every time: enter (33.5, 44.25), motion
 * (120.75, 60.5) */
static M10Ptr
m10_pointer_seq (GdkWindow *w)
{
  M10Ptr r;
  GdkEvent *ev;
  GList *l;
  GdkModifierType mask;

  memset (&r, 0, sizeof r);
  m6_clear ();
  M10_HOOK (_gdk_wayland_test_pointer_enter (m10_d, w, 33.5, 44.25), "pointer enter");
  m6_drain ();
  m10_time += 20;
  M10_HOOK (_gdk_wayland_test_pointer_motion (m10_d, m10_time, 120.75, 60.5), "pointer motion");
  m6_drain ();

  ev = m6_find (GDK_ENTER_NOTIFY, w);
  if (ev != NULL)
    {
      r.got_enter = TRUE;
      r.ex = ev->crossing.x;
      r.ey = ev->crossing.y;
    }
  for (l = m6_events.head; l; l = l->next)
    {
      ev = l->data;
      if (ev->type == GDK_MOTION_NOTIFY && ev->any.window == w)
	{
	  r.got_motion = TRUE;
	  r.mx = ev->motion.x;
	  r.my = ev->motion.y;
	  r.mxr = ev->motion.x_root;
	  r.myr = ev->motion.y_root;
	}
    }
  gdk_window_get_pointer (w, &r.px, &r.py, &mask);
  _gdk_wayland_test_pointer_leave (m10_d, w);
  m6_drain ();

  return r;
}

static void
m10_pointer_check (const M10Ptr *r, const char *tag)
{
  char msg[96];

  g_snprintf (msg, sizeof msg, "T13 %s: got an enter", tag);
  M10_CHECK (r->got_enter, msg);
  g_snprintf (msg, sizeof msg, "T13 %s: got a motion", tag);
  M10_CHECK (r->got_motion, msg);
  g_snprintf (msg, sizeof msg, "T13 %s: enter x (%g)", tag, r->ex);
  /* crossing events carry whole pixels (the core synthesizes them), the
   * motion event keeps the fraction */
  M10_CHECK (r->ex == 33.0, msg);
  g_snprintf (msg, sizeof msg, "T13 %s: enter y (%g)", tag, r->ey);
  M10_CHECK (r->ey == 44.0, msg);
  g_snprintf (msg, sizeof msg, "T13 %s: motion x (%g)", tag, r->mx);
  M10_CHECK (r->mx == 120.75, msg);
  g_snprintf (msg, sizeof msg, "T13 %s: motion y (%g)", tag, r->my);
  M10_CHECK (r->my == 60.5, msg);
  g_snprintf (msg, sizeof msg, "T13 %s: gdk_window_get_pointer x", tag);
  M10_EQ_INT (r->px, 120, msg);
  g_snprintf (msg, sizeof msg, "T13 %s: gdk_window_get_pointer y", tag);
  M10_EQ_INT (r->py, 60, msg);
}

/* ---- T12 integer scale 2 ------------------------------------------------------------ */

static void
m10_t12 (void)
{
  GdkWindow *w = m10_w;
  GDKWTestScaleInfo s0, s1;
  GDKWTestFrameInfo fi;
  GDKWTestBackingInfo bi;
  GdkEvent *ev;
  M10SlotWait sw;
  guint mism = 99;
  gint ox, oy;

  /* WAITING plus HOLD: no commit can sneak in before the checks */
  m10_set_fmode (GDKW_TEST_FRAME_MODE_HOLD);
  m10_redraw (w, 0, 0, 10, 10, 0x204060);
  m10_reset (w);
  s0 = m10_si (w);
  m6_clear ();

  m10_out2 = m10_fake_out (0, 0, 2048, 1280, 2, 0);
  M10_HOOK (_gdk_wayland_test_surface_enter (w, m10_out2), "T12: surface_enter");
  m10_events ();

  s1 = m10_si (w);
  M10_EQ_INT (s1.scale120, 240, "T12: scale120 240");
  M10_EQ_INT (s1.source, GDKW_TEST_SCALE_OUTPUT, "T12: source OUTPUT");
  bi = m10_bi (w);
  M10_EQ_INT (bi.px_width, 400, "T12: store width 400");
  M10_EQ_INT (bi.px_height, 240, "T12: store height 240");
  M10_EQ_INT (bi.stride, 1600, "T12: stride 1600");
  M10_CHECK (bi.device_scale_x == 2.0 && bi.device_scale_y == 2.0, "T12: device scale 2");
  M10_EQ_INT (bi.logical_width, 200, "T12: logical width 200");
  M10_EQ_INT (bi.logical_height, 120, "T12: logical height 120");
  M10_EQ_INT (gdk_window_get_width (w), 200, "T12: window width unchanged");
  M10_EQ_INT (gdk_window_get_height (w), 120, "T12: window height unchanged");
  M10_EQ_INT (m6_count (GDK_CONFIGURE, w), 1, "T12: one GDK_CONFIGURE");
  ev = m6_find (GDK_CONFIGURE, w);
  if (ev != NULL)
    {
      gdk_window_get_position (w, &ox, &oy);
      M10_EQ_INT (ev->configure.x, ox, "T12: event x");
      M10_EQ_INT (ev->configure.y, oy, "T12: event y");
      M10_EQ_INT (ev->configure.width, 200, "T12: event width");
      M10_EQ_INT (ev->configure.height, 120, "T12: event height");
    }
  M10_EQ_INT (s1.buffer_scale_sent, s0.buffer_scale_sent, "T12: set_buffer_scale not sent yet");
  M10_EQ_INT (s1.buffer_scale_requests, s0.buffer_scale_requests, "T12: no request before the commit");

  /* let the redraw and settle idles run: HOLD keeps every commit deferred */
  m10_run (60);
  m10_pump (m10_has_held, w, 300);
  fi = m10_fi (w);
  M10_EQ_INT (fi.commits, 0, "T12: still no commit while WAITING");
  s1 = m10_si (w);
  M10_EQ_INT (s1.buffer_scale_requests, s0.buffer_scale_requests,
	      "T12: still no scale request before the commit");
  M10_CHECK (m6_count (GDK_EXPOSE, w) >= 1, "T12: the window was redrawn (invalidated)");

  m10_fire (w);
  fi = m10_fi (w);
  s1 = m10_si (w);
  M10_EQ_INT (fi.commits, 1, "T12: the callback commits once");
  M10_EQ_INT (fi.last_attach_width, 400, "T12: attach width 400");
  M10_EQ_INT (fi.last_attach_height, 240, "T12: attach height 240");
  M10_EQ_INT (s1.buffer_scale_sent, 2, "T12: set_buffer_scale 2 in the commit");
  M10_EQ_INT (s1.buffer_scale_requests, s0.buffer_scale_requests + 1,
	      "T12: exactly one scale request");
  M10_EQ_INT (s1.viewport_dest_w, -1, "T12: viewport destination unset (w)");
  M10_EQ_INT (s1.viewport_dest_h, -1, "T12: viewport destination unset (h)");
  M10_HOOK (_gdk_wayland_test_buffer_vs_backing (w, fi.last_slot, &mism), "T12: vs_backing");
  M10_EQ_INT (mism, 0, "T12: the buffer equals the store");

  m10_set_fmode (GDKW_TEST_FRAME_MODE_NORMAL);
  sw.w = w;
  sw.width = 200;
  sw.height = 120;
  M10_CHECK (m10_pump (m10_no_slots_cond, &sw, 800), "T12: no 200 x 120 buffer is left");
  gdk_display_sync (m10_d);
  m6_drain ();
  m10_quiet (w);
  m10_paint = -1;
}

/* ---- T14 logical drawing at scale 2, T21 get_image ----------------------------------- */

static void
m10_t14 (void)
{
  GdkWindow *w = m10_w, *c;
  GdkWindowAttr attr;
  GDKWTestBackingInfo bi;
  cairo_t *cr;
  cairo_surface_t *target;
  gdouble ox = -1.0, oy = -1.0;
  guint32 px = 0;
  gint x, y;
  gboolean all_red = TRUE;

  cr = gdk_cairo_create (w);
  cairo_set_source_rgb (cr, 1.0, 0.0, 0.0);
  cairo_rectangle (cr, 10, 10, 20, 20);
  cairo_fill (cr);
  cairo_surface_flush (cairo_get_target (cr));
  cairo_destroy (cr);

  bi = m10_bi (w);
  for (y = 20; y <= 59; y++)
    for (x = 20; x <= 59; x++)
      if (m10_bpx (&bi, x, y) != M10_RED)
	all_red = FALSE;
  M10_CHECK (all_red, "T14: device pixels 20..59 are red");
  M10_CHECK (m10_bpx (&bi, 19, 19) != M10_RED, "T14: (19,19) is not red");
  M10_CHECK (m10_bpx (&bi, 60, 60) != M10_RED, "T14: (60,60) is not red");
  M10_CHECK (m10_bpx (&bi, 19, 40) != M10_RED, "T14: (19,40) is not red");
  M10_CHECK (m10_bpx (&bi, 40, 60) != M10_RED, "T14: (40,60) is not red");
  M10_HOOK (_gdk_wayland_test_window_pixel (w, 10, 10, &px), "T14: window_pixel");
  M10_EQ_HEX (px, M10_RED, "T14: window pixel (10,10) is red (logical argument)");

  /* a client side child at (30, 40): device offset (60, 80), C1 */
  memset (&attr, 0, sizeof attr);
  attr.window_type = GDK_WINDOW_CHILD;
  attr.wclass = GDK_INPUT_OUTPUT;
  attr.x = 30;
  attr.y = 40;
  attr.width = 40;
  attr.height = 40;
  attr.event_mask = GDK_EXPOSURE_MASK;
  c = gdk_window_new (w, &attr, GDK_WA_X | GDK_WA_Y);
  M10_CHECK (c != NULL, "T14: child created");
  if (c != NULL)
    {
      gdk_window_show (c);
      cr = gdk_cairo_create (c);
      target = cairo_get_target (cr);
      cairo_set_source_rgb (cr, 1.0, 0.0, 0.0);
      cairo_rectangle (cr, 0, 0, 5, 5);
      cairo_fill (cr);
      cairo_surface_flush (target);
      cairo_surface_get_device_offset (target, &ox, &oy);
      cairo_destroy (cr);
      M10_CHECK (ox == 60.0 && oy == 80.0, "T14: the child surface has device offset (60, 80)");

      bi = m10_bi (w);
      all_red = TRUE;
      for (y = 80; y <= 89; y++)
	for (x = 60; x <= 69; x++)
	  if (m10_bpx (&bi, x, y) != M10_RED)
	    all_red = FALSE;
      M10_CHECK (all_red, "T14: the child rect lands at device 60..69 x 80..89");
      M10_CHECK (m10_bpx (&bi, 59, 80) != M10_RED, "T14: child (59,80) is not red");
      M10_CHECK (m10_bpx (&bi, 70, 90) != M10_RED, "T14: child (70,90) is not red");
      M10_CHECK (m10_bpx (&bi, 60, 79) != M10_RED, "T14: child (60,79) is not red");
      gdk_window_destroy (c);
      m6_drain ();
    }
}

static void
m10_t21_image (void)
{
  GdkWindow *w = m10_w;
  GdkImage *img = gdk_drawable_get_image (w, 0, 0, 200, 120);

  M10_CHECK (img != NULL, "T21: get_image returned an image");
  if (img == NULL)
    return;
  M10_EQ_INT (gdk_image_get_width (img), 200, "T21: image width is logical");
  M10_EQ_INT (gdk_image_get_height (img), 120, "T21: image height is logical");
  M10_EQ_HEX (gdk_image_get_pixel (img, 10, 10) & 0xffffff, 0xff0000,
	      "T21: image pixel (10,10) is the red of T14");
  M10_EQ_HEX (gdk_image_get_pixel (img, 25, 25) & 0xffffff, 0xff0000,
	      "T21: image pixel (25,25) is red");
  M10_CHECK ((gdk_image_get_pixel (img, 5, 5) & 0xffffff) != 0xff0000,
	     "T21: image pixel (5,5) is not red");
  g_object_unref (img);
}

static int
m10_pred_left (int col, int row)
{
  return col < 8;
}

static void
m10_t21_shape (void)
{
  static const GdkRectangle want[1] = { { 4, 4, 8, 16 } };
  GdkWindow *w = m10_w;
  GdkBitmap *bm = m7_bitmap (16, 16, m10_pred_left);
  GDKWTestShapeInfo si;
  GDKWTestBackingInfo bi;
  guint base;

  M10_CHECK (bm != NULL, "T21: shape bitmap");
  if (bm == NULL)
    return;
  gdk_window_shape_combine_mask (w, bm, 4, 4);
  m6_drain ();
  memset (&si, 0, sizeof si);
  M10_HOOK (_gdk_wayland_test_shape_info (w, &si), "T21: shape_info");
  M10_CHECK (si.shaped, "T21: shaped");
  M10_CHECK (m7_rects_equal (si.bounding, si.n_bounding, want, 1),
	     "T21: bounding region is the logical rectangle");
  M10_CHECK (si.input_sent && m7_rects_equal (si.input, si.n_input, want, 1),
	     "T21: input region is the logical rectangle");

  base = m10_counter (w, M10_COMMITS);
  _gdk_wayland_test_frame_present (w);
  m10_wait_counter (w, M10_COMMITS, base + 1, "T21: commit after the shape");
  bi = m10_bi (w);
  M10_EQ_HEX (m10_bpx (&bi, 0, 0), 0x00000000, "T21: device (0,0) outside the shape is transparent");
  M10_EQ_HEX (m10_bpx (&bi, 1, 1), 0x00000000, "T21: device (1,1) outside the shape is transparent");
  M10_EQ_HEX (m10_bpx (&bi, 100, 10) & 0xff000000u, 0x00000000,
	      "T21: device (100,10) right of the shape is transparent");
  M10_EQ_HEX (m10_bpx (&bi, 12, 12) >> 24, 0xff, "T21: device (12,12) inside the shape is opaque");
  M10_EQ_HEX (m10_bpx (&bi, 23, 39) >> 24, 0xff, "T21: device (23,39) inside the shape is opaque");
  M10_EQ_HEX (m10_bpx (&bi, 24, 12) >> 24, 0x00, "T21: device (24,12) just outside is transparent");

  gdk_window_shape_combine_mask (w, NULL, 0, 0);
  m6_drain ();
  g_object_unref (bm);
  m10_quiet (w);
}

/* ---- T20 popups --------------------------------------------------------------------- */

static GdkWindow *
m10_popup (GdkWindow *transient, gint x, gint y, gint w, gint h)
{
  GdkWindowAttr attr;
  GdkWindow *win;

  memset (&attr, 0, sizeof attr);
  attr.window_type = GDK_WINDOW_TEMP;
  attr.wclass = GDK_INPUT_OUTPUT;
  attr.x = x;
  attr.y = y;
  attr.width = w;
  attr.height = h;
  attr.override_redirect = TRUE;
  attr.event_mask = GDK_STRUCTURE_MASK | GDK_EXPOSURE_MASK |
    GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK | GDK_POINTER_MOTION_MASK;
  win = gdk_window_new (gdk_get_default_root_window (), &attr,
			GDK_WA_X | GDK_WA_Y | GDK_WA_NOREDIR);
  M10_CHECK (win != NULL, "popup created");
  if (win == NULL)
    return NULL;
  gdk_window_set_type_hint (win, GDK_WINDOW_TYPE_HINT_POPUP_MENU);
  gdk_window_set_transient_for (win, transient);

  return win;
}

static gboolean
m10_popup_mapped (gpointer p)
{
  GDKWTestPopupInfo i;

  memset (&i, 0, sizeof i);
  return _gdk_wayland_test_popup_info (p, &i) && i.state == GDKW_TEST_POPUP_MAPPED;
}

/* shows a popup under @parent and waits until it is MAPPED */
static GdkWindow *
m10_popup_shown (GdkWindow *parent)
{
  GdkWindow *p = m10_popup (parent, 50, 60, 80, 40);

  if (p == NULL)
    return NULL;
  gdk_window_show (p);
  gdk_flush ();
  m6_drain ();
  M10_CHECK (m10_pump (m10_popup_mapped, p, 800), "popup: MAPPED");

  return p;
}

typedef struct
{
  GdkWindow *w;
  gint sent;
} M10ScaleSentWait;

static gboolean
m10_scale_sent_cond (gpointer p)
{
  M10ScaleSentWait *sw = p;
  GDKWTestScaleInfo s;

  memset (&s, 0, sizeof s);
  return _gdk_wayland_test_scale_info (sw->w, &s) && s.buffer_scale_sent == sw->sent;
}

static GDKWTestPopupInfo m10_popup_ref;

/* scale 1 reference of the popup positioner values */
static void
m10_t20_ref (void)
{
  GdkWindow *p = m10_popup_shown (m10_w);

  if (p == NULL)
    return;
  m10_popup_ref = m10_pi (p);
  M10_EQ_INT (m10_popup_ref.pos_width, 80, "T20: reference positioner width");
  M10_EQ_INT (m10_popup_ref.pos_height, 40, "T20: reference positioner height");
  m10_destroy (p);
}

static void
m10_t20 (void)
{
  GdkWindow *p = m10_popup_shown (m10_w);
  GDKWTestPopupInfo pi;
  GDKWTestBackingInfo bi;
  M10ScaleSentWait sw;

  if (p == NULL)
    return;
  m10_enter (p, m10_out2);
  pi = m10_pi (p);
  M10_EQ_INT (pi.pos_width, m10_popup_ref.pos_width, "T20: positioner width equals scale 1");
  M10_EQ_INT (pi.pos_height, m10_popup_ref.pos_height, "T20: positioner height equals scale 1");
  M10_EQ_INT (pi.anchor_x, m10_popup_ref.anchor_x, "T20: anchor x equals scale 1");
  M10_EQ_INT (pi.anchor_y, m10_popup_ref.anchor_y, "T20: anchor y equals scale 1");
  M10_EQ_INT (pi.anchor_w, m10_popup_ref.anchor_w, "T20: anchor w equals scale 1");
  M10_EQ_INT (pi.anchor_h, m10_popup_ref.anchor_h, "T20: anchor h equals scale 1");
  M10_EQ_INT (pi.offset_x, m10_popup_ref.offset_x, "T20: offset x equals scale 1");
  M10_EQ_INT (pi.offset_y, m10_popup_ref.offset_y, "T20: offset y equals scale 1");
  M10_EQ_INT (pi.root_x, m10_popup_ref.root_x, "T20: root x equals scale 1");
  M10_EQ_INT (pi.root_y, m10_popup_ref.root_y, "T20: root y equals scale 1");
  M10_EQ_INT (pi.conf_w, m10_popup_ref.conf_w, "T20: configure width equals scale 1");
  M10_EQ_INT (pi.conf_h, m10_popup_ref.conf_h, "T20: configure height equals scale 1");
  bi = m10_bi (p);
  M10_EQ_INT (bi.px_width, 160, "T20: popup store width 160");
  M10_EQ_INT (bi.px_height, 80, "T20: popup store height 80");
  M10_EQ_INT (bi.logical_width, 80, "T20: popup logical width 80");

  m10_redraw (p, 0, 0, 80, 40, 0x884400);
  sw.w = p;
  sw.sent = 2;
  M10_CHECK (m10_pump (m10_scale_sent_cond, &sw, 800), "T20: set_buffer_scale 2 on the popup");
  m10_paint = -1;
  m10_destroy (p);
}

/* ---- T19 cursor scale ---------------------------------------------------------------- */

/* an Xcursor file with a 24 and a 48 pixel image, hotspots (3, 5) and (7, 9) */
static gboolean
m10_write_xcursor2 (const gchar *path)
{
  static const int sizes[2] = { 24, 48 };
  static const int hots[2][2] = { { 3, 5 }, { 7, 9 } };
  GByteArray *a = g_byte_array_new ();
  guint32 off = 16 + 2 * 12;
  gboolean ok;
  int k, i;

  m7_put32 (a, 0x72756358);	/* "Xcur" */
  m7_put32 (a, 16);
  m7_put32 (a, 0x00010000);
  m7_put32 (a, 2);
  for (k = 0; k < 2; k++)
    {
      m7_put32 (a, 0xfffd0002);
      m7_put32 (a, sizes[k]);
      m7_put32 (a, off);
      off += 36 + 4 * sizes[k] * sizes[k];
    }
  for (k = 0; k < 2; k++)
    {
      m7_put32 (a, 36);
      m7_put32 (a, 0xfffd0002);
      m7_put32 (a, sizes[k]);
      m7_put32 (a, 1);
      m7_put32 (a, sizes[k]);
      m7_put32 (a, sizes[k]);
      m7_put32 (a, hots[k][0]);
      m7_put32 (a, hots[k][1]);
      m7_put32 (a, 0);
      for (i = 0; i < sizes[k] * sizes[k]; i++)
	m7_put32 (a, 0xff000000u);
    }
  ok = g_file_set_contents (path, (const gchar *) a->data, a->len, NULL);
  g_byte_array_free (a, TRUE);

  return ok;
}

static GDKWTestCursorScaleInfo
m10_cs (void)
{
  GDKWTestCursorScaleInfo i;

  memset (&i, 0, sizeof i);
  M10_HOOK (_gdk_wayland_test_cursor_scale_info (m10_d, &i), "cursor_scale_info");
  return i;
}

static GDKWTestCursorInfo
m10_ci (void)
{
  GDKWTestCursorInfo i;

  memset (&i, 0, sizeof i);
  M10_HOOK (_gdk_wayland_test_cursor_info (m10_d, &i), "cursor_info");
  return i;
}

/* the focus window W is at scale 2; ends with W back at scale 1 (left out2) */
static void
m10_t19 (void)
{
  GdkWindow *w = m10_w;
  GDKWTestCursorScaleInfo cs;
  GDKWTestCursorInfo ci;
  GdkPixbuf *pb;
  GdkCursor *cur;
  gchar *tmp, *dir, *file, *empty;
  guint sets;

  tmp = g_dir_make_tmp ("ydk-m10-XXXXXX", NULL);
  M10_CHECK (tmp != NULL, "T19: temp dir");
  if (tmp == NULL)
    return;
  dir = g_build_filename (tmp, "ydktest2", "cursors", NULL);
  empty = g_build_filename (tmp, "empty", NULL);
  g_mkdir_with_parents (dir, 0700);
  g_mkdir_with_parents (empty, 0700);
  file = g_build_filename (dir, "left_ptr", NULL);
  M10_CHECK (m10_write_xcursor2 (file), "T19: write left_ptr with 24 and 48 pixel images");

  M10_HOOK (_gdk_wayland_test_cursor_reset (m10_d), "T19: cursor reset");
  M10_HOOK (_gdk_wayland_test_pointer_enter (m10_d, w, 10.0, 10.0), "T19: pointer enter");
  m6_drain ();
  M10_HOOK (_gdk_wayland_test_cursor_theme_reload (m10_d, "ydktest2", 24, tmp), "T19: theme reload");
  m6_drain ();
  cs = m10_cs ();
  ci = m10_ci ();
  M10_EQ_INT (ci.kind, GDKW_TEST_CURSOR_THEME, "T19: a theme cursor is applied");
  M10_EQ_INT (cs.buffer_scale, 2, "T19: buffer scale 2");
  M10_EQ_INT (cs.buffer_width, 48, "T19: buffer width 48");
  M10_EQ_INT (cs.buffer_height, 48, "T19: buffer height 48");
  M10_EQ_INT (cs.theme_px, 48, "T19: theme loaded at 48");
  M10_EQ_INT (cs.hot_x_sent, (7 + 1) / 2, "T19: hotspot x is (hot + 1) / 2");
  M10_EQ_INT (cs.hot_y_sent, (9 + 1) / 2, "T19: hotspot y is (hot + 1) / 2");

  /* a pixbuf cursor stays at scale 1 with its own hotspot */
  pb = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, 16, 16);
  M10_CHECK (pb != NULL, "T19: pixbuf");
  if (pb != NULL)
    {
      gdk_pixbuf_fill (pb, 0xff0000ff);
      cur = gdk_cursor_new_from_pixbuf (m10_d, pb, 3, 4);
      M10_CHECK (cur != NULL, "T19: pixbuf cursor");
      if (cur != NULL)
	{
	  gdk_window_set_cursor (w, cur);
	  m6_drain ();
	  cs = m10_cs ();
	  ci = m10_ci ();
	  M10_EQ_INT (ci.kind, GDKW_TEST_CURSOR_CUSTOM, "T19: a custom cursor is applied");
	  M10_EQ_INT (cs.buffer_scale, 1, "T19: custom cursor buffer scale 1");
	  M10_EQ_INT (cs.hot_x_sent, 3, "T19: custom hotspot x unchanged");
	  M10_EQ_INT (cs.hot_y_sent, 4, "T19: custom hotspot y unchanged");
	  M10_EQ_INT (cs.buffer_width, 16, "T19: custom buffer width");
	  gdk_window_set_cursor (w, NULL);
	  m6_drain ();
	  gdk_cursor_unref (cur);
	}
      g_object_unref (pb);
    }
  cs = m10_cs ();
  M10_EQ_INT (cs.buffer_scale, 2, "T19: the theme cursor is back at scale 2");

  /* W goes to scale 1: the cursor reloads and applies again */
  ci = m10_ci ();
  sets = ci.set_count;
  m10_leave (w, m10_out2);
  m6_drain ();
  cs = m10_cs ();
  ci = m10_ci ();
  M10_EQ_INT (cs.buffer_scale, 1, "T19: scale 1 after the window left the output");
  M10_EQ_INT (cs.theme_px, 24, "T19: theme reloaded at 24");
  M10_EQ_INT (cs.buffer_width, 24, "T19: buffer width 24");
  M10_EQ_INT (cs.hot_x_sent, 3, "T19: hotspot x at scale 1");
  M10_EQ_INT (cs.hot_y_sent, 5, "T19: hotspot y at scale 1");
  M10_CHECK (ci.set_count > sets, "T19: the cursor was applied again");

  _gdk_wayland_test_pointer_leave (m10_d, w);
  m6_drain ();
  M10_HOOK (_gdk_wayland_test_cursor_theme_reload (m10_d, "ydktest", 24, empty), "T19: back to the builtin theme");
  g_remove (file);
  g_free (file);
  g_rmdir (dir);
  file = g_build_filename (tmp, "ydktest2", NULL);
  g_rmdir (file);
  g_free (file);
  g_rmdir (empty);
  g_rmdir (tmp);
  g_free (dir);
  g_free (empty);
  g_free (tmp);
  m10_quiet (w);
}

/* ---- T15 back to scale 1 ---------------------------------------------------------------- */

static void
m10_t15 (void)
{
  GdkWindow *w = m10_w;
  GDKWTestScaleInfo s;
  GDKWTestBackingInfo bi;
  M10ScaleSentWait sw;

  m10_leave (w, m10_out2);
  M10_HOOK (_gdk_wayland_test_output_remove (m10_d, m10_out2), "T15: remove the output");
  m10_out2 = 0;
  m10_events ();
  s = m10_si (w);
  M10_EQ_INT (s.scale120, 120, "T15: scale120 120");
  bi = m10_bi (w);
  M10_EQ_INT (bi.px_width, 200, "T15: store width 200");
  M10_EQ_INT (bi.px_height, 120, "T15: store height 120");
  M10_CHECK (bi.device_scale_x == 1.0 && bi.device_scale_y == 1.0, "T15: device scale 1");

  m10_commit_now (w);
  sw.w = w;
  sw.sent = 1;
  M10_CHECK (m10_pump (m10_scale_sent_cond, &sw, 800), "T15: buffer_scale_sent 1 after the commit");
  m10_quiet (w);
}

/* ---- T16 multi output ------------------------------------------------------------------- */

static void
m10_t16 (void)
{
  GdkWindow *w = m10_w;
  GDKWTestScaleInfo s;
  GDKWTestBackingInfo bi;
  guint real = m10_real_id (), id2, id3;

  M10_CHECK (real != 0, "T16: a real output exists");
  id2 = m10_fake_out (0, 0, 2048, 1280, 2, 0);
  m10_enter (w, real);
  m10_enter (w, id2);
  s = m10_si (w);
  M10_EQ_INT (s.scale120, 240, "T16: both entered gives the maximum, 240");

  m10_leave (w, id2);
  s = m10_si (w);
  M10_EQ_INT (s.scale120, 120, "T16: leaving the scale 2 output gives 120");

  m10_enter (w, id2);
  M10_HOOK (_gdk_wayland_test_output_scale (m10_d, id2, 3), "T16: output scale 3");
  s = m10_si (w);
  M10_EQ_INT (s.scale120, 360, "T16: output scale 3 gives 360");
  bi = m10_bi (w);
  M10_EQ_INT (bi.px_width, 600, "T16: store width 600");
  M10_EQ_INT (bi.px_height, 360, "T16: store height 360");

  M10_HOOK (_gdk_wayland_test_output_remove (m10_d, id2), "T16: remove output 2");
  s = m10_si (w);
  M10_EQ_INT (s.scale120, 120, "T16: after the removal 120 again");
  M10_CHECK (!m10_is_entered (w, id2), "T16: the removed output is no longer entered");
  bi = m10_bi (w);
  M10_EQ_INT (bi.px_width, 200, "T16: store width 200 again");

  /* not entered anywhere: the hint, the largest output scale */
  m10_leave (w, real);
  id3 = m10_fake_out (0, 0, 2048, 1280, 2, 0);
  s = m10_si (w);
  M10_EQ_INT (s.n_entered, 0, "T16: not entered anywhere");
  M10_EQ_INT (s.scale120, 240, "T16: the hint is the largest output scale");
  M10_EQ_INT (s.source, GDKW_TEST_SCALE_HINT, "T16: source HINT");
  M10_HOOK (_gdk_wayland_test_output_remove (m10_d, id3), "T16: remove output 3");
  s = m10_si (w);
  M10_EQ_INT (s.scale120, 120, "T16: hint 120 once the big output is gone");
  m10_enter (w, real);
  m10_events ();
  m10_commit_now (w);
  m10_quiet (w);
}

/* ---- T17 fractional ------------------------------------------------------------------------ */

static void
m10_t17 (void)
{
  GdkWindow *w = m10_w, *w2;
  GDKWTestScaleInfo s, s0;
  GDKWTestBackingInfo bi;

  M10_HOOK (_gdk_wayland_test_fractional_force (m10_d, TRUE), "T17: force on");
  s0 = m10_si (w);
  M10_HOOK (_gdk_wayland_test_preferred_fractional (w, 180), "T17: preferred 180");
  s = m10_si (w);
  M10_EQ_INT (s.scale120, 180, "T17: scale120 180");
  M10_EQ_INT (s.source, GDKW_TEST_SCALE_FRACTIONAL, "T17: source FRACTIONAL");
  bi = m10_bi (w);
  M10_EQ_INT (bi.px_width, 300, "T17: store width 300");
  M10_EQ_INT (bi.px_height, 180, "T17: store height 180");
  M10_CHECK (bi.device_scale_x == 1.5 && bi.device_scale_y == 1.5, "T17: device scale 1.5");
  m10_commit_now (w);
  s = m10_si (w);
  M10_EQ_INT (s.buffer_scale_sent, 1, "T17: buffer scale 1 on the viewport path");
  M10_EQ_INT (s.viewport_dest_w, 200, "T17: viewport destination width");
  M10_EQ_INT (s.viewport_dest_h, 120, "T17: viewport destination height");
  M10_EQ_INT (s.viewport_requests, s0.viewport_requests + 1, "T17: one viewport request");
  gdk_display_sync (m10_d);
  m6_drain ();

  /* the rounding rule: 101 x 51 at 1.5 */
  w2 = m10_new (101, 51);
  if (w2 != NULL)
    {
      M10_HOOK (_gdk_wayland_test_preferred_fractional (w2, 180), "T17: preferred 180 on 101 x 51");
      bi = m10_bi (w2);
      M10_EQ_INT (bi.px_width, GDKW_TEST_SCALE_PX (101, 180), "T17: 101 gives the macro width");
      M10_EQ_INT (bi.px_width, 152, "T17: 101 gives 152");
      M10_EQ_INT (bi.px_height, 77, "T17: 51 gives 77");
      m10_commit_now (w2);
      s = m10_si (w2);
      M10_EQ_INT (s.viewport_dest_w, 101, "T17: destination 101");
      M10_EQ_INT (s.viewport_dest_h, 51, "T17: destination 51");
      m10_destroy (w2);
    }

  /* 240 through the fractional event takes the integer path */
  M10_HOOK (_gdk_wayland_test_preferred_fractional (w, 240), "T17: preferred 240");
  s = m10_si (w);
  M10_EQ_INT (s.scale120, 240, "T17: scale120 240");
  m10_commit_now (w);
  s = m10_si (w);
  M10_EQ_INT (s.viewport_dest_w, -1, "T17: integer path: destination unset");
  M10_EQ_INT (s.buffer_scale_sent, 2, "T17: integer path: buffer scale 2");

  /* without the manager the fractional value is ignored */
  M10_HOOK (_gdk_wayland_test_fractional_force (m10_d, FALSE), "T17: force off");
  M10_HOOK (_gdk_wayland_test_preferred_fractional (w, 180), "T17: preferred 180 without a manager");
  s = m10_si (w);
  M10_CHECK (s.scale120 != 180 && s.source != GDKW_TEST_SCALE_FRACTIONAL,
	     "T17: priority falls through to the output or the hint");

  M10_HOOK (_gdk_wayland_test_preferred_buffer_scale (w, 3), "T17: preferred_buffer_scale 3");
  s = m10_si (w);
  M10_EQ_INT (s.scale120, 360, "T17: preferred_buffer_scale 3 gives 360");
  M10_EQ_INT (s.source, GDKW_TEST_SCALE_PREFERRED_INT, "T17: source PREFERRED_INT");

  /* this window keeps stale preferred values: do not reuse it */
  m10_paint = -1;
  m10_destroy (w);
  m10_w = NULL;
}

/* ---- T18 screen and monitors ------------------------------------------------------------- */

static void
m10_on_size_changed (GdkScreen *screen, gpointer data)
{
  m10_size_changed++;
}

static void
m10_on_monitors_changed (GdkScreen *screen, gpointer data)
{
  m10_monitors_changed++;
}

static void
m10_t18 (void)
{
  GdkScreen *screen = gdk_display_get_default_screen (m10_d);
  GDKWTestOutputInfo oi;
  GdkRectangle g;
  gdouble res0, res1;
  gint rw = 0, rh = 0, sw0, sh0;
  guint real = m10_real_id (), n, i, id3, id4;

  memset (&oi, 0, sizeof oi);
  n = _gdk_wayland_test_output_count (m10_d);
  for (i = 0; i < n; i++)
    if (_gdk_wayland_test_output_info (m10_d, i, &oi) && oi.id == real)
      break;
  rw = oi.logical_width;
  rh = oi.logical_height;
  M10_CHECK (oi.scale >= 1 && rw == oi.mode_width / oi.scale && rh == oi.mode_height / oi.scale,
	     "T18: the logical size is mode / scale");

  M10_EQ_INT (gdk_screen_get_n_monitors (screen), 1, "T18: one monitor");
  gdk_screen_get_monitor_geometry (screen, 0, &g);
  M10_CHECK (g.x == oi.x && g.y == oi.y && g.width == rw && g.height == rh,
	     "T18: monitor 0 geometry is the logical output");
  sw0 = gdk_screen_get_width (screen);
  sh0 = gdk_screen_get_height (screen);
  M10_EQ_INT (sw0, oi.x + rw, "T18: screen width");
  M10_EQ_INT (sh0, oi.y + rh, "T18: screen height");
  res0 = gdk_screen_get_resolution (screen);

  g_signal_connect (screen, "size-changed", G_CALLBACK (m10_on_size_changed), NULL);
  g_signal_connect (screen, "monitors-changed", G_CALLBACK (m10_on_monitors_changed), NULL);

  /* a second output to the right, 1600 x 1200 at scale 2: 800 x 600 */
  m10_size_changed = m10_monitors_changed = 0;
  id3 = m10_fake_out (rw, 0, 1600, 1200, 2, 0);
  M10_EQ_INT (gdk_screen_get_n_monitors (screen), 2, "T18: two monitors");
  gdk_screen_get_monitor_geometry (screen, 1, &g);
  M10_CHECK (g.x == rw && g.y == 0 && g.width == 800 && g.height == 600,
	     "T18: monitor 1 geometry (rw, 0, 800, 600)");
  M10_EQ_INT (gdk_screen_get_width (screen), rw + 800, "T18: screen width is the bounding box");
  M10_EQ_INT (gdk_screen_get_height (screen), MAX (rh, 600), "T18: screen height is the bounding box");
  M10_EQ_INT (m10_size_changed, 1, "T18: one size-changed per done");
  M10_EQ_INT (m10_monitors_changed, 1, "T18: one monitors-changed per done");
  res1 = gdk_screen_get_resolution (screen);
  M10_CHECK (res0 == res1, "T18: resolution unchanged");

  M10_HOOK (_gdk_wayland_test_output_remove (m10_d, id3), "T18: remove output 3");
  M10_EQ_INT (gdk_screen_get_n_monitors (screen), 1, "T18: one monitor again");
  M10_EQ_INT (gdk_screen_get_width (screen), sw0, "T18: screen width back");
  M10_EQ_INT (gdk_screen_get_height (screen), sh0, "T18: screen height back");

  /* transform 90 swaps width and height */
  id4 = m10_fake_out (rw, 0, 1600, 1200, 2, 1);
  M10_EQ_INT (gdk_screen_get_n_monitors (screen), 2, "T18: two monitors for the rotated output");
  gdk_screen_get_monitor_geometry (screen, 1, &g);
  M10_CHECK (g.width == 600 && g.height == 800, "T18: transform 90 swaps width and height");
  M10_HOOK (_gdk_wayland_test_output_remove (m10_d, id4), "T18: remove the rotated output");
  M10_EQ_INT (gdk_screen_get_n_monitors (screen), 1, "T18: one monitor at the end");
  M10_CHECK (gdk_screen_get_resolution (screen) == res0, "T18: resolution still unchanged");

  g_signal_handlers_disconnect_by_func (screen, G_CALLBACK (m10_on_size_changed), NULL);
  g_signal_handlers_disconnect_by_func (screen, G_CALLBACK (m10_on_monitors_changed), NULL);
}

/* ---- T22 scale change while hidden or WAITING ---------------------------------------------- */

static void
m10_t22 (void)
{
  GdkWindow *w = m10_new (200, 120);
  GDKWTestScaleInfo s0, s1;
  GDKWTestFrameInfo fi;
  GDKWTestBackingInfo bi;
  guint out2, real = m10_real_id ();

  if (w == NULL)
    return;
  out2 = m10_fake_out (0, 0, 2048, 1280, 2, 0);
  m10_enter (w, real);

  /* hidden: no event, no commit; the buffer is 2x after the show */
  gdk_window_hide (w);
  gdk_flush ();
  m6_drain ();
  m6_clear ();
  m10_reset (w);
  M10_HOOK (_gdk_wayland_test_surface_enter (w, out2), "T22: enter while hidden");
  m10_events ();
  M10_EQ_INT (m6_count (GDK_CONFIGURE, w), 0, "T22: hidden: no GDK_CONFIGURE");
  fi = m10_fi (w);
  M10_EQ_INT (fi.commits, 0, "T22: hidden: no commit");
  M10_EQ_INT (m10_si (w).scale120, 240, "T22: hidden: the scale is 240");
  gdk_window_show (w);
  m10_wait_counter (w, M10_COMMITS, 1, "T22: commit after the show");
  fi = m10_fi (w);
  M10_EQ_INT (fi.last_attach_width, 400, "T22: the first buffer after the show is 2x (w)");
  M10_EQ_INT (fi.last_attach_height, 240, "T22: the first buffer after the show is 2x (h)");
  M10_EQ_INT (m10_si (w).buffer_scale_sent, 2, "T22: buffer scale 2 sent");
  m10_quiet (w);

  /* WAITING: the new scale and the new buffer go out together */
  m10_set_fmode (GDKW_TEST_FRAME_MODE_HOLD);
  m10_redraw (w, 0, 0, 10, 10, 0x204060);	/* commit now, WAITING */
  m10_reset (w);
  s0 = m10_si (w);
  m10_leave (w, out2);
  m10_redraw (w, 0, 0, 10, 10, 0x204060);
  m10_run (60);
  fi = m10_fi (w);
  M10_EQ_INT (fi.commits, 0, "T22: WAITING: the commit stays deferred");
  s1 = m10_si (w);
  M10_EQ_INT (s1.scale120, 120, "T22: WAITING: scale 120");
  M10_EQ_INT (s1.buffer_scale_requests, s0.buffer_scale_requests,
	      "T22: WAITING: no request before the commit");
  m10_fire (w);
  fi = m10_fi (w);
  s1 = m10_si (w);
  M10_EQ_INT (fi.commits, 1, "T22: WAITING: exactly one commit after the fire");
  M10_EQ_INT (fi.last_attach_width, 200, "T22: WAITING: the new buffer width");
  M10_EQ_INT (s1.buffer_scale_requests, s0.buffer_scale_requests + 1,
	      "T22: WAITING: the scale request went with the buffer");
  M10_EQ_INT (s1.buffer_scale_sent, 1, "T22: WAITING: buffer scale 1");

  /* resize and scale change together: one commit of the right size */
  m10_reset (w);
  s0 = m10_si (w);
  M10_HOOK (_gdk_wayland_test_toplevel_configure (w, 260, 150, 0), "T22: configure 260 x 150");
  M10_HOOK (_gdk_wayland_test_surface_enter (w, out2), "T22: enter with the resize");
  m10_events ();
  m10_run (60);
  fi = m10_fi (w);
  M10_EQ_INT (fi.commits, 0, "T22: resize + scale: deferred");
  m10_fire (w);
  fi = m10_fi (w);
  s1 = m10_si (w);
  bi = m10_bi (w);
  M10_EQ_INT (fi.commits, 1, "T22: resize + scale: one commit");
  M10_EQ_INT (fi.last_attach_width, 520, "T22: resize + scale: width 520");
  M10_EQ_INT (fi.last_attach_height, 300, "T22: resize + scale: height 300");
  M10_EQ_INT (bi.px_width, 520, "T22: resize + scale: store width");
  M10_EQ_INT (s1.buffer_scale_sent, 2, "T22: resize + scale: buffer scale 2");
  M10_EQ_INT (s1.buffer_scale_requests, s0.buffer_scale_requests + 1,
	      "T22: resize + scale: one scale request");

  m10_set_fmode (GDKW_TEST_FRAME_MODE_NORMAL);
  m10_leave (w, out2);
  M10_HOOK (_gdk_wayland_test_output_remove (m10_d, out2), "T22: remove the output");
  m10_paint = -1;
  m10_destroy (w);
}

/* ---- T24 cycles ------------------------------------------------------------------------------- */

typedef struct
{
  GDKWTestBufferCounts bc;
  GDKWTestFrameCounts fc;
  GDKWTestObjectCounts oc;
} M10Snap;

static M10Snap m10_snap_base;

static void
m10_snap_take (M10Snap *s)
{
  memset (s, 0, sizeof *s);
  _gdk_wayland_test_buffer_counts (m10_d, &s->bc);
  _gdk_wayland_test_frame_counts (m10_d, &s->fc);
  _gdk_wayland_test_object_counts (m10_d, &s->oc);
}

static gboolean
m10_snap_back (gpointer p)
{
  M10Snap s;

  m10_snap_take (&s);
  return s.bc.live_wl_buffers <= m10_snap_base.bc.live_wl_buffers &&
    s.bc.live_stores <= m10_snap_base.bc.live_stores &&
    s.fc.timers <= m10_snap_base.fc.timers &&
    s.fc.callbacks <= m10_snap_base.fc.callbacks &&
    s.fc.settle_idles <= m10_snap_base.fc.settle_idles &&
    s.oc.wl_surfaces == m10_snap_base.oc.wl_surfaces &&
    s.oc.xdg_surfaces == m10_snap_base.oc.xdg_surfaces &&
    s.oc.xdg_toplevels == m10_snap_base.oc.xdg_toplevels &&
    s.oc.registered_windows == m10_snap_base.oc.registered_windows;
}

static void
m10_t24 (void)
{
  M10Snap now;
  guint out, real = m10_real_id ();
  int i;

  gdk_window_process_all_updates ();
  m10_run (100);
  m10_snap_take (&m10_snap_base);
  out = m10_fake_out (0, 0, 2048, 1280, 2, 0);

  for (i = 0; i < 50; i++)
    {
      GdkWindow *w = m10_new_ex (100 + i % 7, 60, FALSE);

      if (w == NULL)
	break;
      m10_redraw (w, 0, 0, 50, 30, 0x3366cc);
      gdk_window_resize (w, 120 + i % 5, 80);
      m10_events ();
      if (i % 2 == 0)
	{
	  m10_enter (w, real);
	  m10_enter (w, out);
	}
      m10_redraw (w, 0, 0, 50, 30, 0xcc6633);
      gdk_window_hide (w);
      if (i % 3 == 0)
	m10_leave (w, out);
      gdk_window_show (w);
      m6_drain ();
      if (i % 4 == 0)
	m10_run (5);
      /* destroyed while a callback, a timer or an idle may be pending */
      gdk_window_destroy (w);
      m6_drain ();
      m6_clear ();
    }

  M10_HOOK (_gdk_wayland_test_output_remove (m10_d, out), "T24: remove the output");
  gdk_display_sync (m10_d);
  m6_drain ();
  M10_CHECK (m10_pump (m10_snap_back, NULL, 1500), "T24: everything is back to the baseline");
  m10_snap_take (&now);
  M10_CHECK (now.bc.live_wl_buffers <= m10_snap_base.bc.live_wl_buffers, "T24: no live wl_buffer left");
  M10_CHECK (now.bc.live_stores <= m10_snap_base.bc.live_stores, "T24: no live store left");
  M10_CHECK (now.fc.timers <= m10_snap_base.fc.timers, "T24: no timer left");
  M10_CHECK (now.fc.callbacks <= m10_snap_base.fc.callbacks, "T24: no frame callback left");
  M10_CHECK (now.fc.settle_idles <= m10_snap_base.fc.settle_idles, "T24: no settle idle left");
  M10_EQ_INT (now.oc.wl_surfaces, m10_snap_base.oc.wl_surfaces, "T24: no wl_surface left");
  M10_EQ_INT (now.oc.xdg_toplevels, m10_snap_base.oc.xdg_toplevels, "T24: no xdg_toplevel left");
  M10_EQ_INT (now.oc.registered_windows, m10_snap_base.oc.registered_windows, "T24: no window left");
  m10_paint = -1;
}

/* ---- the default weston run ---------------------------------------------------------------------- */

static void
m10_restore_modes (void)
{
  m10_fmode = GDKW_TEST_FRAME_MODE_NORMAL;
  m10_bmode = GDKW_TEST_BUFFER_MODE_NORMAL;
  _gdk_wayland_test_frame_mode (m10_d, GDKW_TEST_FRAME_MODE_NORMAL);
  _gdk_wayland_test_buffer_mode (m10_d, GDKW_TEST_BUFFER_MODE_NORMAL);
  _gdk_wayland_test_buffer_limit (m10_d, 0);
  _gdk_wayland_test_frame_timeouts (m10_d, 0, 0);
  _gdk_wayland_test_fractional_force (m10_d, FALSE);
  m10_paint = -1;
}

/* the window of the scale group: T20 reference, T13, T12, T14, T21, T20, T19,
 * T15, T16, T17 */
static void
m10_scale_group (void)
{
  M10Ptr p1, p2;

  m10_w = m10_new (200, 120);
  if (m10_w == NULL)
    return;
  M10_CHECK (m10_pump (m10_entered_any, m10_w, 300), "scale group: the real enter arrived");

  m10_t20_ref ();
  p1 = m10_pointer_seq (m10_w);
  m10_pointer_check (&p1, "scale 1");

  m10_t12 ();
  p2 = m10_pointer_seq (m10_w);
  m10_pointer_check (&p2, "scale 2");
  M10_CHECK (p1.mxr == p2.mxr && p1.myr == p2.myr, "T13: x_root and y_root equal the scale 1 run");

  m10_t14 ();
  m10_t21_image ();
  m10_t21_shape ();
  m10_t20 ();
  m10_t19 ();
  m10_t15 ();
  m10_t16 ();
  m10_t17 ();
  m10_destroy (m10_w);
  m10_w = NULL;
}

static int
m10_exercise (void)
{
  m10_d = gdk_display_get_default ();
  m10_failures = 0;
  m10_checks = 0;

  gdk_event_handler_set (m10_handler, NULL, NULL);
  m6_clear ();
  gdk_window_process_all_updates ();
  m10_run (100);

  m10_t1 ();
  m10_t2 ();
  m10_t3 ();
  m10_t4 ();
  m10_t5 ();
  m10_t6 (0);
  m10_t6 (2);
  m10_t7 ();
  m10_t8 ();
  m10_t9 ();
  m10_t10 ();
  m10_t11 ();
  m10_restore_modes ();
  m10_scale_group ();
  m10_restore_modes ();
  m10_t18 ();
  m10_t22 ();
  m10_restore_modes ();
  m10_t24 ();
  m10_restore_modes ();

  m6_clear ();
  gdk_event_handler_set (event_handler, NULL, NULL);
  gdk_window_invalidate_rect (main_window, NULL, TRUE);

  g_print ("ydk_demo: M10: %d checks run\n", m10_checks);
  if (m10_failures)
    g_printerr ("ydk_demo: M10: FAIL (%d)\n", m10_failures);
  else
    g_print ("ydk_demo: M10: PASS\n");

  return m10_failures;
}

/* ---- T23: weston --scale=2, no scale hooks except where stated -------------------------------------- */

static void
m10_real_exercise_body (void)
{
  GdkScreen *screen = gdk_display_get_default_screen (m10_d);
  GDKWTestOutputInfo oi;
  GDKWTestScaleInfo s, sm;
  GDKWTestFrameInfo fi;
  GDKWTestBackingInfo bi;
  GdkRectangle g;
  GdkWindow *w;
  M10Ptr p;
  guint real = m10_real_id (), base, i;
  gint k;

  memset (&oi, 0, sizeof oi);
  for (i = 0; i < _gdk_wayland_test_output_count (m10_d); i++)
    if (_gdk_wayland_test_output_info (m10_d, i, &oi) && oi.id == real)
      break;
  M10_EQ_INT (oi.scale, 2, "T23: the real output has scale 2 (start weston with --scale=2)");
  M10_CHECK (oi.logical_width == oi.mode_width / 2 && oi.logical_height == oi.mode_height / 2,
	     "T23: the logical size is mode / 2");
  M10_EQ_INT (gdk_screen_get_n_monitors (screen), 1, "T23: one monitor");
  gdk_screen_get_monitor_geometry (screen, 0, &g);
  M10_CHECK (g.x == 0 && g.y == 0 && g.width == oi.logical_width && g.height == oi.logical_height,
	     "T23: monitor 0 geometry is the logical output");
  M10_EQ_INT (g.width, 1024, "T23: logical width 1024");
  M10_EQ_INT (g.height, 640, "T23: logical height 640");
  M10_EQ_INT (gdk_screen_get_width (screen), g.width, "T23: screen width is logical");

  m10_wait_counter (main_window, M10_COMMITS, 1, "T23: the main window committed");
  M10_CHECK (m10_pump (m10_entered_any, main_window, 800), "T23: the main window entered the output");
  sm = m10_si (main_window);
  M10_EQ_INT (sm.scale120, 240, "T23: the main window has scale 240");
  M10_EQ_INT (sm.source, GDKW_TEST_SCALE_OUTPUT, "T23: its source is OUTPUT (the real enter)");

  /* a new window: hint at the first commit, set_buffer_scale 2 with it */
  m10_set_fmode (GDKW_TEST_FRAME_MODE_HOLD);
  w = m10_new_ex (200, 120, FALSE);
  if (w == NULL)
    {
      m10_set_fmode (GDKW_TEST_FRAME_MODE_NORMAL);
      return;
    }
  fi = m10_fi (w);
  s = m10_si (w);
  M10_EQ_INT (fi.commits, 1, "T23: the first commit");
  M10_EQ_INT (s.scale120, 240, "T23: scale 240 at the first commit (hint)");
  M10_EQ_INT (s.buffer_scale_sent, 2, "T23: set_buffer_scale 2 went out with the first buffer");
  M10_CHECK (s.buffer_scale_requests >= 1, "T23: a scale request was made");
  M10_EQ_INT (fi.last_attach_width, 400, "T23: first buffer width is twice the logical width");
  M10_EQ_INT (fi.last_attach_height, 240, "T23: first buffer height is twice the logical height");
  bi = m10_bi (w);
  M10_EQ_INT (bi.px_width, 400, "T23: the backing is twice the logical width");
  M10_EQ_INT (bi.px_height, 240, "T23: the backing is twice the logical height");
  M10_CHECK (bi.device_scale_x == 2.0 && bi.device_scale_y == 2.0, "T23: device scale 2");
  m10_fire (w);
  m10_set_fmode (GDKW_TEST_FRAME_MODE_NORMAL);
  M10_CHECK (m10_pump (m10_entered_any, w, 800), "T23: the real enter arrived");
  s = m10_si (w);
  M10_EQ_INT (s.scale120, 240, "T23: scale 240 after the enter");
  M10_EQ_INT (s.source, GDKW_TEST_SCALE_OUTPUT, "T23: source OUTPUT after the enter");
  m10_quiet (w);
  m10_reset (w);

  /* ten paced commits with real callbacks */
  for (k = 0; k < 10; k++)
    {
      m10_redraw (w, 0, 0, 200, 120, 0x102030 + 0x101010 * (k % 8));
      m10_wait_counter (w, M10_CB_DONE, k + 1, "T23: paced frame callback");
    }
  fi = m10_fi (w);
  M10_CHECK (fi.commits >= 10, "T23: ten commits");
  M10_EQ_INT (fi.frame_timeouts, 0, "T23: no timeout");
  base = fi.commits;
  gdk_display_sync (m10_d);
  m6_drain ();
  fi = m10_fi (w);
  M10_CHECK (fi.commits >= base, "T23: weston is still serving us");

  /* pointer coordinates (hook) are unchanged */
  p = m10_pointer_seq (w);
  m10_pointer_check (&p, "real scale 2");

  /* a fractional scale switches to the viewport path */
  M10_HOOK (_gdk_wayland_test_fractional_force (m10_d, TRUE), "T23: force on");
  M10_HOOK (_gdk_wayland_test_preferred_fractional (w, 180), "T23: preferred 180");
  bi = m10_bi (w);
  M10_EQ_INT (bi.px_width, 300, "T23: fractional store width 300");
  m10_commit_now (w);
  s = m10_si (w);
  M10_EQ_INT (s.buffer_scale_sent, 1, "T23: fractional: buffer scale 1");
  M10_EQ_INT (s.viewport_dest_w, 200, "T23: fractional: destination width");
  M10_EQ_INT (s.viewport_dest_h, 120, "T23: fractional: destination height");
  gdk_display_sync (m10_d);
  m6_drain ();
  M10_HOOK (_gdk_wayland_test_fractional_force (m10_d, FALSE), "T23: force off");

  m10_paint = -1;
  m10_destroy (w);
}

static int
m10_real_exercise (void)
{
  m10_d = gdk_display_get_default ();
  m10_failures = 0;
  m10_checks = 0;

  gdk_event_handler_set (m10_handler, NULL, NULL);
  m6_clear ();
  gdk_window_process_all_updates ();
  m10_run (100);

  m10_real_exercise_body ();

  m10_restore_modes ();
  m6_clear ();
  gdk_event_handler_set (event_handler, NULL, NULL);
  gdk_window_invalidate_rect (main_window, NULL, TRUE);

  g_print ("ydk_demo: M10-REAL2: %d checks run\n", m10_checks);
  if (m10_failures)
    g_printerr ("ydk_demo: M10-REAL2: FAIL (%d)\n", m10_failures);
  else
    g_print ("ydk_demo: M10-REAL2: PASS\n");

  return m10_failures;
}
