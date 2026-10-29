/*
 * gdkselection-wayland.c
 *
 * ydk-wayland fork: Wayland backend counterpart of x11/gdkselection-x11.c,
 * namespaced `_gdk_wayland_*` so both backends can live in one libydk.
 * The canonical public names are owned by ydk/gdkbackend.c.
 *
 * M8, piece "selection".  Selection ownership, paste, copy serving, native
 * ids and the mime map.  The model is the GDK (X11) property protocol: every
 * transfer is a (window, property) pair plus a requestor token, so a copy and
 * a paste inside one process work with no compositor at all (loopback).  The
 * wl_data_* and zwp_primary_selection_* requests are made by the datadevice
 * piece only; this file talks to it through the functions of gdkm8-wayland.h.
 * See the DECISIONS in that header (OWNERSHIP, CONVERT, SERVING, MIME MAP,
 * NATIVE IDS).
 *
 * The text-property helpers at the end need no windowing system and are
 * implemented with plain GLib charset conversion (no Xlib, no COMPOUND_TEXT).
 */

#include "config.h"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>

#include <ydk/gdk.h>
#include "gdkprivate-wayland.h"
#include "gdkseat-wayland.h"
#include "gdkm8-wayland.h"

#ifdef GDK_COMPILATION
#include "ydk/gdkalias.h"
#endif

#define NATIVE_ID_KEY "ydk-wayland-native-id"

/* ---------------------------------------------------------------------------
 * Records
 * ------------------------------------------------------------------------ */

typedef struct _State  State;
typedef struct _SelRec SelRec;
typedef struct _Op     Op;

typedef enum
{
  NID_WINDOW = 0,
  NID_SEND,
  NID_PROBE,
  NID_DISCARD
} NidKind;

typedef enum
{
  OP_LOCAL = 0,   /* loopback request waiting for the local owner */
  OP_RECV,        /* external offer being read from a pipe */
  OP_SEND,        /* compositor asked our source, owner being asked */
  OP_PROBE,       /* TARGETS probe of a new owner */
  OP_DISCARD      /* request whose answer is thrown away */
} OpKind;

typedef struct
{
  guint32    id;
  NidKind    kind;
  GdkWindow *window;   /* NID_WINDOW only; kept with g_object_weak_ref () */
  Op        *op;       /* the other kinds */
  State     *st;
} NidEntry;

/* One per selection atom ever used on the display; lives until the display
 * state is freed so the counters survive an owner change. */
struct _SelRec
{
  State      *st;
  GdkAtom     selection;
  GDKWSelKind kind;
  GdkWindow  *owner;        /* weak pointer */
  gboolean    has_owner;
  guint32     time;
  guint       generation;
  GDKWSource *source;       /* wl source of the current generation, or NULL */
  GArray     *map;          /* its source map (GDKWMimeMap rows), or NULL */
  Op         *probe;        /* outstanding TARGETS probe, or NULL */
  guint       clear_events;
  guint       probes_started;
  guint       probes_completed;
};

struct _Op
{
  State      *st;
  OpKind      kind;
  guint32     token;        /* native id entry for SEND / PROBE / DISCARD */
  GdkAtom     selection;
  GdkAtom     target;
  GdkAtom     property;
  guint32     time;
  GdkWindow  *owner;        /* weak pointer */
  GdkWindow  *requestor;    /* weak pointer (LOCAL, RECV) */
  guint       timeout_id;

  /* PROBE, SEND */
  SelRec     *rec;
  guint       generation;

  /* SEND */
  GDKWConv    conv;
  int         fd;           /* -1 once handed to the writer */
  int         slot;         /* -1 when none */
  GBytes     *bytes;        /* held until the write completed or was aborted */
  gboolean    writing;

  /* SEND (writer), RECV (reader): owned by the datadevice piece */
  GDKWPipeOp *wop;

  /* RECV */
  GDKWOffer  *offer;        /* reference held */
  GDKWConv    rconv;
  GdkAtom     rtype;
  gboolean    utf8_fix;     /* UTF8_STRING: bytes that are not UTF-8 are Latin-1 */
};

struct _State
{
  GdkDisplay *display;      /* no reference */
  GHashTable *ids;          /* guint32 -> NidEntry */
  guint32     next_id;
  GHashTable *recs;         /* GdkAtom -> SelRec */
  GList      *ops;          /* every live Op, creation order */
  gboolean    slot_used[GDKW_MAX_SEND_SLOTS];
};

static GList *all_states = NULL;

/* ---------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------ */

GdkAtom
_gdk_wayland_selection_property_atom (void)
{
  return _gdk_wayland_atom_intern_static_string ("GDK_SELECTION");
}

GdkAtom
_gdk_wayland_selection_dnd_atom (void)
{
  return _gdk_wayland_atom_intern_static_string ("XdndSelection");
}

GDKWSelKind
_gdk_wayland_selection_kind (GdkAtom selection)
{
  if (selection == GDK_NONE)
    return GDKW_SEL_NONE;
  if (selection == GDK_SELECTION_CLIPBOARD)
    return GDKW_SEL_CLIPBOARD;
  if (selection == GDK_SELECTION_PRIMARY)
    return GDKW_SEL_PRIMARY;
  if (selection == _gdk_wayland_selection_dnd_atom ())
    return GDKW_SEL_DND;
  return GDKW_SEL_NONE;
}

static GdkAtom
atom_named (const gchar *name)
{
  return _gdk_wayland_atom_intern (name, FALSE);
}

/* A live Wayland window: not destroyed and with a Wayland impl. */
static gboolean
window_alive (GdkWindow *window)
{
  return window != NULL && GDK_IS_WINDOW (window) &&
         !gdk_window_is_destroyed (window) &&
         _gdk_wayland_window_get_impl (window) != NULL;
}

static void
weak_set (GdkWindow **slot, GdkWindow *window)
{
  *slot = window;
  if (window != NULL)
    g_object_add_weak_pointer (G_OBJECT (window), (gpointer *) slot);
}

static void
weak_clear (GdkWindow **slot)
{
  if (*slot != NULL)
    g_object_remove_weak_pointer (G_OBJECT (*slot), (gpointer *) slot);
  *slot = NULL;
}

static void
ensure_sigpipe_ignored (void)
{
  struct sigaction sa;

  if (sigaction (SIGPIPE, NULL, &sa) == 0 &&
      !(sa.sa_flags & SA_SIGINFO) && sa.sa_handler == SIG_DFL)
    signal (SIGPIPE, SIG_IGN);
}

static guint
timeout_or (guint value, guint fallback)
{
  return value != 0 ? value : fallback;
}

/* ---------------------------------------------------------------------------
 * Events
 * ------------------------------------------------------------------------ */

static void
put_selection_event (GdkEventType    type,
                     GdkWindow      *window,
                     GdkAtom         selection,
                     GdkAtom         target,
                     GdkAtom         property,
                     guint32         time,
                     GdkNativeWindow requestor)
{
  GdkEvent *event;

  if (!window_alive (window))
    return;

  event = gdk_event_new (type);
  event->selection.window = g_object_ref (window);
  event->selection.send_event = FALSE;
  event->selection.selection = selection;
  event->selection.target = target;
  event->selection.property = property;
  event->selection.time = time;
  event->selection.requestor = requestor;
  gdk_event_set_screen (event, gdk_drawable_get_screen (GDK_DRAWABLE (window)));

  gdk_event_put (event);
  gdk_event_free (event);
}

static void
put_notify (GdkWindow *requestor,
            GdkAtom    selection,
            GdkAtom    target,
            GdkAtom    property,
            guint32    time)
{
  put_selection_event (GDK_SELECTION_NOTIFY, requestor, selection, target,
                       property, time, 0);
}

/* ---------------------------------------------------------------------------
 * State, records and operations
 * ------------------------------------------------------------------------ */

static void op_free (Op *op, gboolean cancel_pipe);

static void
nid_window_gone (gpointer data, GObject *where)
{
  NidEntry *e = data;

  /* the object is going away: do not unref the weak reference again */
  e->window = NULL;
  g_hash_table_remove (e->st->ids, GUINT_TO_POINTER (e->id));
}

static void
nid_entry_free (gpointer p)
{
  NidEntry *e = p;

  if (e->kind == NID_WINDOW && e->window != NULL)
    g_object_weak_unref (G_OBJECT (e->window), nid_window_gone, e);
  g_slice_free (NidEntry, e);
}

static void
rec_free (gpointer p)
{
  SelRec *rec = p;

  weak_clear (&rec->owner);
  if (rec->map != NULL)
    _gdk_wayland_selection_source_map_free (rec->map);
  g_slice_free (SelRec, rec);
}

static State *
state_peek (GdkDisplay *display)
{
  if (display == NULL || !GDK_IS_DISPLAY (display))
    return NULL;
  return g_object_get_data (G_OBJECT (display), GDKW_SELECTION_DATA_KEY);
}

static void state_detach (State *st, gboolean full);

static void
state_free (gpointer data)
{
  State *st = data;

  state_detach (st, TRUE);
  all_states = g_list_remove (all_states, st);
  g_hash_table_destroy (st->recs);
  g_hash_table_destroy (st->ids);
  g_slice_free (State, st);
}

static State *
state_get (GdkDisplay *display)
{
  State *st;

  if (display == NULL || !GDK_IS_DISPLAY (display) ||
      !GDKW_IS_DISPLAY_WAYLAND (display))
    return NULL;

  st = state_peek (display);
  if (st == NULL)
    {
      st = g_slice_new0 (State);
      st->display = display;
      st->next_id = GDKW_NATIVE_ID_FIRST;
      st->ids = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
                                       nid_entry_free);
      st->recs = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
                                        rec_free);
      all_states = g_list_prepend (all_states, st);
      g_object_set_data_full (G_OBJECT (display), GDKW_SELECTION_DATA_KEY, st,
                              state_free);
    }

  return st;
}

static SelRec *
rec_find (State *st, GdkAtom selection)
{
  return g_hash_table_lookup (st->recs, selection);
}

static SelRec *
rec_get (State *st, GdkAtom selection)
{
  SelRec *rec = rec_find (st, selection);

  if (rec == NULL)
    {
      rec = g_slice_new0 (SelRec);
      rec->st = st;
      rec->selection = selection;
      rec->kind = _gdk_wayland_selection_kind (selection);
      g_hash_table_insert (st->recs, selection, rec);
    }

  return rec;
}

static GDKWDataDevice *
dd_of (State *st)
{
  return _gdk_wayland_display_get_data_device (st->display);
}

static GDKWSeat *
seat_of (State *st)
{
  return _gdk_wayland_display_get_seat (st->display);
}

/* ---- native ids ---------------------------------------------------------- */

static guint32
nid_next (State *st)
{
  for (;;)
    {
      guint32 id = st->next_id;

      st->next_id = id + 1;
      if (st->next_id < GDKW_NATIVE_ID_FIRST)
        st->next_id = GDKW_NATIVE_ID_FIRST;

      if (id >= GDKW_NATIVE_ID_FIRST &&
          !g_hash_table_contains (st->ids, GUINT_TO_POINTER (id)))
        return id;
    }
}

static NidEntry *
nid_add (State *st, NidKind kind, GdkWindow *window, Op *op)
{
  NidEntry *e = g_slice_new0 (NidEntry);

  e->st = st;
  e->id = nid_next (st);
  e->kind = kind;
  e->op = op;
  if (kind == NID_WINDOW)
    {
      e->window = window;
      g_object_weak_ref (G_OBJECT (window), nid_window_gone, e);
    }
  g_hash_table_insert (st->ids, GUINT_TO_POINTER (e->id), e);

  return e;
}

GdkNativeWindow
_gdk_wayland_native_id_for_window (GdkWindow *window)
{
  State *st;
  NidEntry *e;
  guint32 id;

  if (!window_alive (window))
    return 0;

  st = state_get (gdk_drawable_get_display (GDK_DRAWABLE (window)));
  if (st == NULL)
    return 0;

  id = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (window), NATIVE_ID_KEY));
  if (id != 0)
    {
      e = g_hash_table_lookup (st->ids, GUINT_TO_POINTER (id));
      if (e != NULL && e->kind == NID_WINDOW && e->window == window)
        return id;
    }

  e = nid_add (st, NID_WINDOW, window, NULL);
  g_object_set_data (G_OBJECT (window), NATIVE_ID_KEY, GUINT_TO_POINTER (e->id));

  return e->id;
}

static GdkWindow *
nid_entry_window (NidEntry *e)
{
  GdkWindow *w = (e->kind == NID_WINDOW) ? e->window :
                 (e->op != NULL ? e->op->owner : NULL);

  return window_alive (w) ? w : NULL;
}

GdkWindow *
_gdk_wayland_native_id_lookup (GdkDisplay *display, GdkNativeWindow id)
{
  State *st;
  NidEntry *e;

  if (display == NULL)
    display = gdk_display_get_default ();

  st = state_peek (display);
  if (st == NULL || id == 0)
    return NULL;

  e = g_hash_table_lookup (st->ids, GUINT_TO_POINTER ((guint32) id));
  return e != NULL ? nid_entry_window (e) : NULL;
}

GdkNativeWindow
_gdk_wayland_test_native_id (GdkWindow *window)
{
  return _gdk_wayland_native_id_for_window (window);
}

/* ---- operations ---------------------------------------------------------- */

static Op *
op_new (State *st, OpKind kind)
{
  Op *op = g_slice_new0 (Op);

  op->st = st;
  op->kind = kind;
  op->fd = -1;
  op->slot = -1;
  st->ops = g_list_append (st->ops, op);

  return op;
}

static void
op_alloc_token (Op *op, NidKind kind)
{
  op->token = nid_add (op->st, kind, NULL, op)->id;
}

static void
op_free (Op *op, gboolean cancel_pipe)
{
  State *st = op->st;

  if (op->timeout_id != 0)
    g_source_remove (op->timeout_id);
  op->timeout_id = 0;

  if (op->wop != NULL && cancel_pipe)
    _gdk_wayland_dd_pipe_cancel (op->wop);
  op->wop = NULL;

  if (op->fd >= 0)
    close (op->fd);
  op->fd = -1;

  if (op->bytes != NULL)
    g_bytes_unref (op->bytes);
  op->bytes = NULL;

  if (op->offer != NULL)
    _gdk_wayland_dd_offer_unref (op->offer);
  op->offer = NULL;

  if (op->slot >= 0)
    st->slot_used[op->slot] = FALSE;

  if (op->rec != NULL && op->rec->probe == op)
    op->rec->probe = NULL;

  if (op->token != 0)
    g_hash_table_remove (st->ids, GUINT_TO_POINTER (op->token));

  weak_clear (&op->owner);
  weak_clear (&op->requestor);

  st->ops = g_list_remove (st->ops, op);
  g_slice_free (Op, op);
}

/* Releases everything the protocol side of @rec holds: the probe, the pending
 * outbound transfers, the source and its map.  No events. */
static void
rec_release_protocol (SelRec *rec, gboolean free_source)
{
  GList *l, *doomed = NULL;
  GDKWSource *src;

  if (rec->probe != NULL)
    op_free (rec->probe, FALSE);

  for (l = rec->st->ops; l != NULL; l = l->next)
    {
      Op *op = l->data;

      if (op->kind == OP_SEND && op->rec == rec)
        doomed = g_list_prepend (doomed, op);
    }
  for (l = doomed; l != NULL; l = l->next)
    op_free (l->data, TRUE);
  g_list_free (doomed);

  src = rec->source;
  rec->source = NULL;
  if (src != NULL && free_source)
    _gdk_wayland_dd_source_free (src);

  if (rec->map != NULL)
    _gdk_wayland_selection_source_map_free (rec->map);
  rec->map = NULL;
}

/* The local owner of @rec goes away without a SELECTION_CLEAR. */
static void
rec_drop_owner (SelRec *rec)
{
  rec_release_protocol (rec, TRUE);
  weak_clear (&rec->owner);
  rec->has_owner = FALSE;

  if (rec->kind == GDKW_SEL_CLIPBOARD || rec->kind == GDKW_SEL_PRIMARY)
    {
      GDKWDataDevice *dd = g_object_get_data (G_OBJECT (rec->st->display),
                                              GDKW_DATADEVICE_DATA_KEY);

      if (dd != NULL)
        _gdk_wayland_dd_forget_self_offer (dd, rec->kind);
    }
}

/* Protocol side of a display going away or a data device being finalized.  The
 * datadevice piece has already destroyed its proxies, sources and pipe
 * operations, so nothing is cancelled or freed on its side here.  @full also
 * drops the loopback operations. */
static void
state_detach (State *st, gboolean full)
{
  GList *l, *copy = g_list_copy (st->ops);
  GHashTableIter iter;
  gpointer key, value;

  for (l = copy; l != NULL; l = l->next)
    {
      Op *op = l->data;

      switch (op->kind)
        {
        case OP_RECV:
          /* the reader was cancelled with the device: the requestor must
           * still get its one NOTIFY */
          put_notify (op->requestor, op->selection, op->target, GDK_NONE,
                      op->time);
          op_free (op, FALSE);
          break;
        case OP_SEND:
        case OP_PROBE:
          op_free (op, FALSE);
          break;
        case OP_LOCAL:
        case OP_DISCARD:
          if (full)
            op_free (op, FALSE);
          break;
        }
    }
  g_list_free (copy);

  g_hash_table_iter_init (&iter, st->recs);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      SelRec *rec = value;

      /* the sources are already freed by the datadevice piece: forget them */
      rec->source = NULL;
      rec->probe = NULL;
      if (rec->map != NULL)
        _gdk_wayland_selection_source_map_free (rec->map);
      rec->map = NULL;
      if (full)
        weak_clear (&rec->owner);
    }
}

void
_gdk_wayland_selection_display_finalize (GdkDisplay *display)
{
  State *st = state_peek (display);

  if (st != NULL)
    state_detach (st, FALSE);
}

/* ---------------------------------------------------------------------------
 * Mime map (pure)
 * ------------------------------------------------------------------------ */

static const gchar * const never_offered[] = {
  "TARGETS", "MULTIPLE", "TIMESTAMP", "SAVE_TARGETS", "DELETE", "ATOM_PAIR",
  "INCR", "NULL", "TEXT", "COMPOUND_TEXT", NULL
};

static gboolean
name_is_pseudo (const gchar *name)
{
  gint i;

  if (name == NULL || name[0] == '\0')
    return TRUE;
  for (i = 0; never_offered[i] != NULL; i++)
    if (strcmp (name, never_offered[i]) == 0)
      return TRUE;
  return FALSE;
}

/* Outbound text group: rank 0 is the best serving target. */
static gint
text_rank (const gchar *name)
{
  if (strcmp (name, "UTF8_STRING") == 0)
    return 0;
  if (strcmp (name, "STRING") == 0)
    return 1;
  if (g_ascii_strcasecmp (name, "text/plain;charset=utf-8") == 0)
    return 2;
  if (strcmp (name, "text/plain") == 0)
    return 3;
  return -1;
}

static void
map_row_clear (gpointer p)
{
  GDKWMimeMap *row = p;

  g_free (row->mime);
}

static gboolean
map_has_mime (const GArray *map, const gchar *mime)
{
  guint i;

  for (i = 0; i < map->len; i++)
    if (strcmp (g_array_index (map, GDKWMimeMap, i).mime, mime) == 0)
      return TRUE;
  return FALSE;
}

static void
map_add (GArray *map, gint pos, const gchar *mime, GdkAtom target,
         GdkAtom type, GDKWConv conv)
{
  GDKWMimeMap row;

  if (map_has_mime (map, mime))
    return;

  row.mime = g_strdup (mime);
  row.target = target;
  row.type = type;
  row.conv = conv;
  if (pos < 0)
    g_array_append_val (map, row);
  else
    g_array_insert_val (map, (guint) pos, row);
}

GArray *
_gdk_wayland_selection_source_map_new (const GdkAtom *targets, gint n_targets)
{
  GArray *map = g_array_new (FALSE, TRUE, sizeof (GDKWMimeMap));
  gint i, text_pos = -1, best_rank = G_MAXINT;
  GdkAtom best = GDK_NONE;

  g_array_set_clear_func (map, map_row_clear);

  if (targets == NULL || n_targets <= 0)
    return map;

  for (i = 0; i < n_targets; i++)
    {
      gchar *name;
      gint rank;

      if (targets[i] == GDK_NONE)
        continue;
      name = _gdk_wayland_atom_name (targets[i]);
      if (name == NULL)
        continue;

      rank = text_rank (name);
      if (rank >= 0)
        {
          if (text_pos < 0)
            text_pos = (gint) map->len;
          if (rank < best_rank)
            {
              best_rank = rank;
              best = targets[i];
            }
        }
      else if (name_is_pseudo (name) ||
               strncmp (name, "text/plain;", 11) == 0)
        ; /* never offered; other charsets of text/plain are ignored */
      else
        map_add (map, -1, name, targets[i], targets[i], GDKW_CONV_RAW);

      g_free (name);
    }

  if (best != GDK_NONE)
    {
      GDKWConv conv = best_rank == 0 ? GDKW_CONV_RAW :
                      best_rank == 1 ? GDKW_CONV_LATIN1_TO_UTF8 :
                                       GDKW_CONV_CRLF_TO_LF;
      GdkAtom type = best_rank == 1 ? GDK_TARGET_STRING :
                     best_rank == 0 ? atom_named ("UTF8_STRING") : best;

      /* inserted in reverse so the final order is utf-8, plain, UTF8_STRING */
      map_add (map, text_pos, "UTF8_STRING", best, type, conv);
      map_add (map, text_pos, "text/plain", best, type, conv);
      map_add (map, text_pos, "text/plain;charset=utf-8", best, type, conv);
    }

  return map;
}

gchar **
_gdk_wayland_selection_source_map_mimes (const GArray *map)
{
  gchar **v;
  guint i;

  if (map == NULL)
    return g_new0 (gchar *, 1);

  v = g_new0 (gchar *, map->len + 1);
  for (i = 0; i < map->len; i++)
    v[i] = g_strdup (g_array_index (map, GDKWMimeMap, i).mime);

  return v;
}

void
_gdk_wayland_selection_source_map_free (GArray *map)
{
  if (map != NULL)
    g_array_free (map, TRUE);
}

gchar **
_gdk_wayland_selection_mimes_for_targets (const GdkAtom *targets,
                                          gint           n_targets)
{
  GArray *map = _gdk_wayland_selection_source_map_new (targets, n_targets);
  gchar **v = _gdk_wayland_selection_source_map_mimes (map);

  _gdk_wayland_selection_source_map_free (map);
  return v;
}

static gboolean
inbound_text_mime (const gchar *mime)
{
  return g_ascii_strcasecmp (mime, "text/plain;charset=utf-8") == 0 ||
         strcmp (mime, "UTF8_STRING") == 0 ||
         strcmp (mime, "text/plain") == 0;
}

GdkAtom *
_gdk_wayland_selection_targets_for_mimes (const gchar * const *mimes,
                                          gint                *n_targets)
{
  GPtrArray *out = g_ptr_array_new ();
  gint text_pos = -1;
  gboolean saw_string = FALSE;
  guint i;
  GdkAtom *result;

  if (n_targets != NULL)
    *n_targets = 0;
  if (mimes == NULL)
    {
      g_ptr_array_free (out, TRUE);
      return NULL;
    }

  for (i = 0; mimes[i] != NULL; i++)
    {
      GdkAtom atom;

      if (name_is_pseudo (mimes[i]))
        continue;

      if (inbound_text_mime (mimes[i]))
        {
          if (text_pos < 0)
            text_pos = (gint) out->len;
          continue;
        }

      atom = atom_named (mimes[i]);
      if (atom == GDK_TARGET_STRING)
        {
          saw_string = TRUE;   /* synthesized from the text group below */
          continue;
        }
      if (atom != GDK_NONE && !g_ptr_array_find (out, atom, NULL))
        g_ptr_array_add (out, atom);
    }

  if (text_pos >= 0)
    {
      /* inserted STRING first so the order ends up UTF8_STRING, STRING */
      g_ptr_array_insert (out, text_pos, GDK_TARGET_STRING);
      g_ptr_array_insert (out, text_pos, atom_named ("UTF8_STRING"));
    }
  else if (saw_string)
    g_ptr_array_add (out, GDK_TARGET_STRING);

  if (out->len == 0)
    {
      g_ptr_array_free (out, TRUE);
      return NULL;
    }

  result = g_new (GdkAtom, out->len);
  for (i = 0; i < out->len; i++)
    result[i] = out->pdata[i];
  if (n_targets != NULL)
    *n_targets = (gint) out->len;
  g_ptr_array_free (out, TRUE);

  return result;
}

GList *
_gdk_wayland_selection_targets_for_offer (const GDKWOffer *offer)
{
  GList *list = NULL;
  gchar **v;
  GdkAtom *atoms;
  gint n, i;
  guint k;

  if (offer == NULL || offer->mimes == NULL)
    return NULL;

  v = g_new0 (gchar *, offer->mimes->len + 1);
  for (k = 0; k < offer->mimes->len; k++)
    v[k] = g_ptr_array_index (offer->mimes, k);

  atoms = _gdk_wayland_selection_targets_for_mimes ((const gchar * const *) v, &n);
  g_free (v);

  for (i = 0; i < n; i++)
    list = g_list_prepend (list, atoms[i]);
  g_free (atoms);

  return g_list_reverse (list);
}

/* Inbound resolution of @target on @offer: the mime to read, the conversion,
 * the type of the stored property.  NULL when the target is refused. */
static const gchar *
inbound_mime (const GDKWOffer *offer,
              GdkAtom          target,
              GDKWConv        *conv,
              GdkAtom         *type,
              gboolean        *utf8_fix)
{
  static const gchar * const order[] = {
    "text/plain;charset=utf-8", "UTF8_STRING", "text/plain", NULL
  };
  gchar *name;
  guint i;
  gint o;
  const gchar *found = NULL;

  *conv = GDKW_CONV_RAW;
  *type = target;
  *utf8_fix = FALSE;

  if (offer == NULL || offer->mimes == NULL || target == GDK_NONE)
    return NULL;
  name = _gdk_wayland_atom_name (target);
  if (name == NULL)
    return NULL;

  if (name_is_pseudo (name))
    {
      g_free (name);
      return NULL;
    }

  if (strcmp (name, "UTF8_STRING") == 0 || strcmp (name, "STRING") == 0)
    {
      for (o = 0; order[o] != NULL && found == NULL; o++)
        for (i = 0; i < offer->mimes->len; i++)
          if (g_ascii_strcasecmp (g_ptr_array_index (offer->mimes, i),
                                  order[o]) == 0)
            {
              found = g_ptr_array_index (offer->mimes, i);
              break;
            }
      if (found != NULL)
        {
          if (strcmp (name, "STRING") == 0)
            *conv = GDKW_CONV_UTF8_TO_LATIN1;
          else
            *utf8_fix = TRUE;
          g_free (name);
          return found;
        }
    }

  for (i = 0; i < offer->mimes->len; i++)
    if (strcmp (g_ptr_array_index (offer->mimes, i), name) == 0)
      {
        found = g_ptr_array_index (offer->mimes, i);
        break;
      }

  g_free (name);
  return found;
}

/* ---------------------------------------------------------------------------
 * Ownership
 * ------------------------------------------------------------------------ */

static GdkAtom
probe_property (GDKWSelKind kind)
{
  return atom_named (kind == GDKW_SEL_PRIMARY ? "YDK_WL_PROBE_PRIMARY" :
                                                "YDK_WL_PROBE_CLIPBOARD");
}

static gboolean
probe_timeout (gpointer data)
{
  Op *op = data;

  op->timeout_id = 0;
  if (op->owner != NULL)
    _gdk_wayland_property_delete (op->owner, op->property);
  op_free (op, FALSE);
  return G_SOURCE_REMOVE;
}

static void
probe_start (SelRec *rec)
{
  State *st = rec->st;
  GDKWDataDevice *dd = dd_of (st);
  Op *op = op_new (st, OP_PROBE);

  op->rec = rec;
  op->generation = rec->generation;
  op->selection = rec->selection;
  op->target = atom_named ("TARGETS");
  op->property = probe_property (rec->kind);
  op->time = seat_of (st)->last_time;
  weak_set (&op->owner, rec->owner);
  op_alloc_token (op, NID_PROBE);
  op->timeout_id = g_timeout_add (timeout_or (dd->probe_timeout_ms,
                                              GDKW_PROBE_TIMEOUT_MS),
                                  probe_timeout, op);
  rec->probe = op;
  rec->probes_started++;

  _gdk_wayland_property_delete (rec->owner, op->property);
  put_selection_event (GDK_SELECTION_REQUEST, rec->owner, rec->selection,
                       op->target, op->property, op->time, op->token);
}

static void
probe_answer (Op *op, GdkAtom property)
{
  SelRec *rec = op->rec;
  State *st = op->st;
  GdkWindow *owner = op->owner;
  gboolean valid;
  GdkAtom *targets = NULL;
  gint n_targets = 0;
  GDKWDataDevice *dd;
  GDKWSelKind kind = rec->kind;
  GArray *map;
  guint32 serial = seat_of (st)->last_serial;

  valid = rec->probe == op && op->generation == rec->generation &&
          rec->has_owner && owner != NULL && owner == rec->owner &&
          window_alive (owner);

  if (window_alive (owner) && property != GDK_NONE)
    {
      GdkAtom type = GDK_NONE;
      gint fmt = 0, len = 0;
      guchar *data = NULL;

      if (_gdk_wayland_property_get (owner, property, GDK_NONE, 0, G_MAXULONG,
                                     TRUE, &type, &fmt, &len, &data) &&
          valid && type == GDK_SELECTION_TYPE_ATOM && fmt == 32 && len > 0)
        {
          n_targets = len / (gint) sizeof (GdkAtom);
          targets = g_memdup2 (data, (gsize) n_targets * sizeof (GdkAtom));
        }
      g_free (data);
    }

  op_free (op, FALSE);
  if (!valid)
    {
      g_free (targets);
      return;
    }

  rec->probes_completed++;

  dd = dd_of (st);
  if (!_gdk_wayland_dd_usable (dd, kind))
    {
      g_free (targets);
      return;
    }

  map = _gdk_wayland_selection_source_map_new (targets, n_targets);
  g_free (targets);

  if (map->len == 0)
    {
      _gdk_wayland_selection_source_map_free (map);
      _gdk_wayland_dd_set_selection (dd, kind, NULL, serial);
    }
  else
    {
      gchar **mimes = _gdk_wayland_selection_source_map_mimes (map);
      GDKWSource *src = _gdk_wayland_dd_source_new (dd, kind,
                                                    (const gchar * const *) mimes,
                                                    _gdk_wayland_selection_source_handlers (),
                                                    rec);

      g_strfreev (mimes);
      if (src == NULL)
        {
          _gdk_wayland_selection_source_map_free (map);
          return;
        }

      rec->source = src;
      rec->map = map;
      _gdk_wayland_dd_set_selection (dd, kind, src, serial);
    }
}

gboolean
_gdk_wayland_selection_owner_set_for_display (GdkDisplay *display,
                                              GdkWindow  *owner,
                                              GdkAtom     selection,
                                              guint32     time,
                                              gboolean    send_event)
{
  State *st;
  SelRec *rec;

  if (display == NULL || !GDK_IS_DISPLAY (display) || selection == GDK_NONE)
    return FALSE;

  st = state_get (display);
  if (st == NULL)
    return FALSE;

  if (owner != NULL &&
      (!window_alive (owner) ||
       gdk_drawable_get_display (GDK_DRAWABLE (owner)) != display))
    return FALSE;

  rec = rec_get (st, selection);

  /* X rule: a request older than the owner's time is ignored */
  if (time != 0 && rec->has_owner && rec->time != 0 && time != rec->time &&
      (guint32) (rec->time - time) < 0x80000000u)
    return FALSE;

  if (owner == NULL)
    {
      if (rec->has_owner)
        rec_drop_owner (rec);
      return TRUE;
    }

  /* replacing an owner in this process sends no SELECTION_CLEAR */
  rec_release_protocol (rec, TRUE);
  weak_clear (&rec->owner);
  weak_set (&rec->owner, owner);
  rec->has_owner = TRUE;
  rec->time = time;
  rec->generation++;

  if ((rec->kind == GDKW_SEL_CLIPBOARD || rec->kind == GDKW_SEL_PRIMARY) &&
      _gdk_wayland_dd_usable (dd_of (st), rec->kind))
    probe_start (rec);

  return TRUE;
}

GdkWindow *
_gdk_wayland_selection_owner_get_for_display (GdkDisplay *display,
                                              GdkAtom     selection)
{
  State *st = state_peek (display);
  SelRec *rec;

  if (st == NULL || selection == GDK_NONE)
    return NULL;

  rec = rec_find (st, selection);
  if (rec != NULL && rec->has_owner && window_alive (rec->owner))
    return rec->owner;

  return NULL;
}

gboolean
_gdk_wayland_selection_has_local_owner (GdkDisplay *display, GDKWSelKind kind)
{
  State *st = state_peek (display);
  SelRec *rec;
  GdkAtom atom;

  if (st == NULL)
    return FALSE;

  atom = kind == GDKW_SEL_CLIPBOARD ? GDK_SELECTION_CLIPBOARD :
         kind == GDKW_SEL_PRIMARY ? GDK_SELECTION_PRIMARY :
         kind == GDKW_SEL_DND ? _gdk_wayland_selection_dnd_atom () : GDK_NONE;
  if (atom == GDK_NONE)
    return FALSE;

  rec = rec_find (st, atom);
  return rec != NULL && rec->has_owner && window_alive (rec->owner);
}

/* wl source events for CLIPBOARD and PRIMARY ---------------------------- */

static void
handler_send (GDKWSource *src, const gchar *mime, int fd)
{
  SelRec *rec = src != NULL ? src->user_data : NULL;

  if (rec == NULL)
    {
      if (fd >= 0)
        close (fd);
      return;
    }

  _gdk_wayland_selection_serve (src, rec->selection, rec->map, mime, fd);
}

static void
handler_cancelled (GDKWSource *src)
{
  SelRec *rec = src != NULL ? src->user_data : NULL;
  GdkWindow *owner;
  guint32 time;

  /* a source the record no longer holds is dead weight: free it, no event */
  if (rec == NULL || rec->source != src)
    {
      _gdk_wayland_dd_source_free (src);
      return;
    }

  /* another client took the selection: drop the ownership (the self offer
   * stays, it may well be the new owner's) and tell the owner once */
  rec->source = NULL;
  _gdk_wayland_dd_source_free (src);

  if (!rec->has_owner)
    return;

  owner = rec->owner;
  time = seat_of (rec->st)->last_time;
  rec_release_protocol (rec, TRUE);
  weak_clear (&rec->owner);
  rec->has_owner = FALSE;

  if (window_alive (owner))
    {
      put_selection_event (GDK_SELECTION_CLEAR, owner, rec->selection,
                           GDK_NONE, GDK_NONE, time, 0);
      rec->clear_events++;
    }
}

static const GDKWSourceHandlers selection_handlers = {
  NULL,               /* target */
  handler_send,
  handler_cancelled,
  NULL,               /* dnd_drop_performed */
  NULL,               /* dnd_finished */
  NULL                /* action */
};

const GDKWSourceHandlers *
_gdk_wayland_selection_source_handlers (void)
{
  return &selection_handlers;
}

/* ---------------------------------------------------------------------------
 * Byte conversions
 * ------------------------------------------------------------------------ */

/* Owner bytes to wire bytes.  Returns a g_malloc'ed buffer with one extra NUL,
 * or NULL when the conversion failed. */
static guchar *
convert_outbound (GDKWConv conv, const guchar *in, gsize len, gsize *out_len)
{
  guchar *buf;

  if (len == 0 || in == NULL)
    {
      *out_len = 0;
      return g_malloc0 (1);
    }

  if (conv == GDKW_CONV_LATIN1_TO_UTF8)
    {
      gsize written = 0;
      gchar *s = g_convert ((const gchar *) in, (gssize) len, "UTF-8",
                            "ISO-8859-1", NULL, &written, NULL);

      if (s == NULL)
        return NULL;
      *out_len = written;
      return (guchar *) s;
    }

  buf = g_malloc (len + 1);
  if (conv == GDKW_CONV_CRLF_TO_LF)
    {
      gsize i, n = 0;

      for (i = 0; i < len; i++)
        {
          if (in[i] == '\r' && i + 1 < len && in[i + 1] == '\n')
            continue;
          buf[n++] = in[i];
        }
      buf[n] = '\0';
      *out_len = n;
    }
  else
    {
      memcpy (buf, in, len);
      buf[len] = '\0';
      *out_len = len;
    }

  return buf;
}

/* Wire bytes to the bytes stored for the requestor.  g_malloc'ed, one extra
 * NUL, never NULL. */
static guchar *
convert_inbound (const Op *op, const guchar *in, gsize len, gsize *out_len)
{
  guchar *buf;

  if (len > 0 && in != NULL)
    {
      if (op->rconv == GDKW_CONV_UTF8_TO_LATIN1)
        {
          gsize written = 0;
          gchar *s = g_convert_with_fallback ((const gchar *) in, (gssize) len,
                                              "ISO-8859-1", "UTF-8", "?",
                                              NULL, &written, NULL);

          if (s != NULL)
            {
              *out_len = written;
              return (guchar *) s;
            }
        }
      else if (op->utf8_fix && !g_utf8_validate ((const gchar *) in, (gssize) len, NULL))
        {
          gsize written = 0;
          gchar *s = g_convert ((const gchar *) in, (gssize) len, "UTF-8",
                                "ISO-8859-1", NULL, &written, NULL);

          if (s != NULL)
            {
              *out_len = written;
              return (guchar *) s;
            }
        }
    }

  buf = g_malloc (len + 1);
  if (len > 0 && in != NULL)
    memcpy (buf, in, len);
  buf[len] = '\0';
  *out_len = len;

  return buf;
}

/* ---------------------------------------------------------------------------
 * Convert (paste)
 * ------------------------------------------------------------------------ */

static gboolean
local_timeout (gpointer data)
{
  Op *op = data;
  GdkWindow *requestor = op->requestor;

  op->timeout_id = 0;
  put_notify (requestor, op->selection, op->target, GDK_NONE, op->time);
  op_free (op, FALSE);
  return G_SOURCE_REMOVE;
}

static void
convert_local (State *st, SelRec *rec, GdkWindow *requestor, GdkAtom selection,
               GdkAtom target, guint32 time)
{
  GDKWDataDevice *dd = dd_of (st);
  Op *op = op_new (st, OP_LOCAL);
  GdkNativeWindow rid = _gdk_wayland_native_id_for_window (requestor);

  op->selection = selection;
  op->target = target;
  op->property = _gdk_wayland_selection_property_atom ();
  op->time = time;
  weak_set (&op->requestor, requestor);
  weak_set (&op->owner, rec->owner);
  op->timeout_id = g_timeout_add (timeout_or (dd->local_timeout_ms,
                                              GDKW_LOCAL_REQUEST_TIMEOUT_MS),
                                  local_timeout, op);

  put_selection_event (GDK_SELECTION_REQUEST, rec->owner, selection, target,
                       op->property, time, rid);
}

static void
recv_done (GDKWReadStatus status, guchar *data, gsize length, gpointer user_data)
{
  Op *op = user_data;
  GdkWindow *requestor = op->requestor;
  GdkAtom prop = GDK_NONE;

  /* the pipe operation is finished and freed by its owner */
  op->wop = NULL;

  if (window_alive (requestor))
    {
      if (status == GDKW_READ_OK && data != NULL)
        {
          gsize out_len = 0;
          guchar *out = convert_inbound (op, data, length, &out_len);

          if (out_len <= GDKW_SELECTION_MAX_BYTES)
            {
              prop = _gdk_wayland_selection_property_atom ();
              _gdk_wayland_property_change (requestor, prop, op->rtype, 8,
                                            GDK_PROP_MODE_REPLACE, out,
                                            (gint) out_len);
            }
          g_free (out);
        }
      put_notify (requestor, op->selection, op->target, prop, op->time);
    }

  g_free (data);
  op_free (op, FALSE);
}

static void
convert_external (State       *st,
                  GdkWindow   *requestor,
                  GdkAtom      selection,
                  GDKWSelKind  kind,
                  GdkAtom      target,
                  guint32      time,
                  GDKWOffer   *offer)
{
  gchar *name = _gdk_wayland_atom_name (target);
  GdkAtom prop = _gdk_wayland_selection_property_atom ();
  GdkAtom answer = GDK_NONE;

  if (name == NULL)
    {
      put_notify (requestor, selection, target, GDK_NONE, time);
      return;
    }

  if (strcmp (name, "TARGETS") == 0)
    {
      GList *list = _gdk_wayland_selection_targets_for_offer (offer);
      gint n = (gint) g_list_length (list), i = 0;
      GdkAtom *atoms = g_new0 (GdkAtom, n + 1);
      GList *l;

      for (l = list; l != NULL; l = l->next)
        atoms[i++] = l->data;
      g_list_free (list);

      _gdk_wayland_property_change (requestor, prop, GDK_SELECTION_TYPE_ATOM,
                                    32, GDK_PROP_MODE_REPLACE,
                                    (const guchar *) atoms, n);
      g_free (atoms);
      answer = prop;
    }
  else if (strcmp (name, "DELETE") == 0 && kind == GDKW_SEL_DND)
    {
      _gdk_wayland_property_change (requestor, prop, atom_named ("NULL"), 8,
                                    GDK_PROP_MODE_REPLACE, NULL, 0);
      answer = prop;
    }
  else
    {
      GDKWConv conv;
      GdkAtom type;
      gboolean fix;
      const gchar *mime = inbound_mime (offer, target, &conv, &type, &fix);

      if (mime != NULL)
        {
          Op *op = op_new (st, OP_RECV);

          op->selection = selection;
          op->target = target;
          op->property = prop;
          op->time = time;
          op->rconv = conv;
          op->rtype = type;
          op->utf8_fix = fix;
          weak_set (&op->requestor, requestor);
          op->offer = _gdk_wayland_dd_offer_ref (offer);
          op->wop = _gdk_wayland_dd_offer_receive (offer, mime, recv_done, op);
          if (op->wop != NULL)
            {
              g_free (name);
              return;
            }
          op_free (op, FALSE);
        }
    }

  g_free (name);
  put_notify (requestor, selection, target, answer, time);
}

void
_gdk_wayland_selection_convert (GdkWindow *requestor,
                                GdkAtom    selection,
                                GdkAtom    target,
                                guint32    time)
{
  State *st;
  SelRec *rec;
  GDKWSelKind kind;
  GDKWDataDevice *dd;
  GDKWOffer *offer = NULL;

  if (selection == GDK_NONE || !window_alive (requestor))
    return;

  st = state_get (gdk_drawable_get_display (GDK_DRAWABLE (requestor)));
  if (st == NULL)
    return;

  /* a. loopback */
  rec = rec_find (st, selection);
  if (rec != NULL && rec->has_owner && window_alive (rec->owner))
    {
      convert_local (st, rec, requestor, selection, target, time);
      return;
    }

  /* b. external offer */
  kind = _gdk_wayland_selection_kind (selection);
  dd = dd_of (st);
  if (kind == GDKW_SEL_CLIPBOARD || kind == GDKW_SEL_PRIMARY)
    offer = dd->selection_offer[kind];
  else if (kind == GDKW_SEL_DND)
    offer = dd->dnd.dest_offer;

  if (offer != NULL && !offer->dead)
    {
      convert_external (st, requestor, selection, kind, target, time, offer);
      return;
    }

  /* c. nothing to ask */
  put_notify (requestor, selection, target, GDK_NONE, time);
}

gint
_gdk_wayland_selection_property_get (GdkWindow  *requestor,
                                     guchar    **data,
                                     GdkAtom    *ret_type,
                                     gint       *ret_format)
{
  GdkAtom type = GDK_NONE;
  gint format = 0, length = 0;
  guchar *buf = NULL;

  if (data)
    *data = NULL;
  if (ret_type)
    *ret_type = GDK_NONE;
  if (ret_format)
    *ret_format = 0;

  if (!window_alive (requestor))
    return 0;

  if (!_gdk_wayland_property_get (requestor,
                                  _gdk_wayland_selection_property_atom (),
                                  GDK_NONE, 0, G_MAXULONG, FALSE,
                                  &type, &format, &length, &buf))
    return 0;

  if (length < 0)
    length = 0;
  if (data)
    {
      /* ytk treats a NULL buffer as failure: always hand out a NUL padded one */
      *data = g_malloc0 ((gsize) length + 1);
      if (length > 0 && buf != NULL)
        memcpy (*data, buf, (gsize) length);
    }
  g_free (buf);

  if (ret_type)
    *ret_type = type;
  if (ret_format)
    *ret_format = format;

  return length;
}

/* ---------------------------------------------------------------------------
 * Serving (copy)
 * ------------------------------------------------------------------------ */

static gboolean
send_timeout (gpointer data)
{
  Op *op = data;

  op->timeout_id = 0;
  if (op->owner != NULL)
    _gdk_wayland_property_delete (op->owner, op->property);
  op_free (op, FALSE);    /* closes the fd */
  return G_SOURCE_REMOVE;
}

static void
send_written (gboolean ok, gpointer user_data)
{
  Op *op = user_data;

  op->wop = NULL;
  op_free (op, FALSE);
}

static void
send_answer (Op *op, GdkAtom property)
{
  GdkWindow *owner = op->owner;
  GdkAtom type = GDK_NONE;
  gint fmt = 0, len = 0;
  guchar *data = NULL;
  guchar *out;
  gsize out_len = 0;
  GDKWDataDevice *dd;
  int fd;

  if (op->writing)
    return;

  if (op->timeout_id != 0)
    g_source_remove (op->timeout_id);
  op->timeout_id = 0;

  if (property == GDK_NONE || !window_alive (owner) ||
      !_gdk_wayland_property_get (owner, op->property, GDK_NONE, 0, G_MAXULONG,
                                  TRUE, &type, &fmt, &len, &data))
    {
      g_free (data);
      op_free (op, FALSE);
      return;
    }

  if (type == atom_named ("INCR"))
    {
      g_debug ("ydk-wayland: INCR reply refused, selection too large for one transfer");
      g_free (data);
      op_free (op, FALSE);
      return;
    }

  out = convert_outbound (fmt == 8 ? op->conv : GDKW_CONV_RAW, data,
                          len > 0 ? (gsize) len : 0, &out_len);
  g_free (data);
  if (out == NULL)
    {
      op_free (op, FALSE);
      return;
    }

  dd = dd_of (op->st);
  ensure_sigpipe_ignored ();

  /* the writer owns the fd from here on and closes it */
  fd = op->fd;
  op->fd = -1;
  op->writing = TRUE;
  op->bytes = g_bytes_new_take (out, out_len);
  op->wop = _gdk_wayland_dd_write_async (fd, op->bytes,
                                         timeout_or (dd->send_timeout_ms,
                                                     GDKW_SEND_TIMEOUT_MS),
                                         send_written, op);
}

void
_gdk_wayland_selection_serve (GDKWSource   *src,
                              GdkAtom       selection,
                              const GArray *map,
                              const gchar  *mime,
                              int           fd)
{
  State *st;
  SelRec *rec;
  GDKWDataDevice *dd;
  const GDKWMimeMap *row = NULL;
  Op *op;
  guint i;
  gint slot;
  gchar *slot_name;

  if (fd < 0)
    return;

  if (src == NULL || src->dd == NULL || src->dd->display == NULL ||
      map == NULL || mime == NULL || selection == GDK_NONE)
    goto refuse;

  st = state_peek ((GdkDisplay *) src->dd->display);
  rec = st != NULL ? rec_find (st, selection) : NULL;
  if (rec == NULL || !rec->has_owner || !window_alive (rec->owner))
    goto refuse;
  if (rec->kind != GDKW_SEL_DND && rec->source != src)
    goto refuse;

  for (i = 0; i < map->len; i++)
    if (strcmp (g_array_index (map, GDKWMimeMap, i).mime, mime) == 0)
      {
        row = &g_array_index (map, GDKWMimeMap, i);
        break;
      }
  if (row == NULL)
    goto refuse;

  for (slot = 0; slot < GDKW_MAX_SEND_SLOTS; slot++)
    if (!st->slot_used[slot])
      break;
  if (slot == GDKW_MAX_SEND_SLOTS)
    goto refuse;

  dd = dd_of (st);
  op = op_new (st, OP_SEND);
  op->rec = rec;
  op->generation = rec->generation;
  op->selection = selection;
  op->target = row->target;
  op->conv = row->conv;
  op->time = seat_of (st)->last_time;
  op->fd = fd;
  op->slot = slot;
  st->slot_used[slot] = TRUE;
  slot_name = g_strdup_printf ("YDK_WL_SEND_%d", slot);
  op->property = atom_named (slot_name);
  g_free (slot_name);
  weak_set (&op->owner, rec->owner);
  op_alloc_token (op, NID_SEND);
  op->timeout_id = g_timeout_add (timeout_or (dd->send_timeout_ms,
                                              GDKW_SEND_TIMEOUT_MS),
                                  send_timeout, op);

  _gdk_wayland_property_delete (op->owner, op->property);
  put_selection_event (GDK_SELECTION_REQUEST, op->owner, selection, op->target,
                       op->property, op->time, op->token);
  return;

refuse:
  close (fd);
}

/* ---------------------------------------------------------------------------
 * Discarded requests (DnD DELETE)
 * ------------------------------------------------------------------------ */

static gboolean
discard_timeout (gpointer data)
{
  Op *op = data;

  op->timeout_id = 0;
  if (op->owner != NULL)
    _gdk_wayland_property_delete (op->owner, op->property);
  op_free (op, FALSE);
  return G_SOURCE_REMOVE;
}

void
_gdk_wayland_selection_discard_request (GdkDisplay *display,
                                        GdkAtom     selection,
                                        GdkAtom     target)
{
  State *st = state_peek (display);
  SelRec *rec;
  GDKWDataDevice *dd;
  Op *op;

  if (st == NULL || selection == GDK_NONE)
    return;
  rec = rec_find (st, selection);
  if (rec == NULL || !rec->has_owner || !window_alive (rec->owner))
    return;

  dd = dd_of (st);
  op = op_new (st, OP_DISCARD);
  op->selection = selection;
  op->target = target;
  op->property = atom_named ("YDK_WL_DISCARD");
  op->time = seat_of (st)->last_time;
  weak_set (&op->owner, rec->owner);
  op_alloc_token (op, NID_DISCARD);
  op->timeout_id = g_timeout_add (timeout_or (dd->local_timeout_ms,
                                              GDKW_LOCAL_REQUEST_TIMEOUT_MS),
                                  discard_timeout, op);

  put_selection_event (GDK_SELECTION_REQUEST, op->owner, selection, target,
                       op->property, op->time, op->token);
}

/* ---------------------------------------------------------------------------
 * Replies from the owner
 * ------------------------------------------------------------------------ */

void
_gdk_wayland_selection_send_notify_for_display (GdkDisplay      *display,
                                                GdkNativeWindow  requestor,
                                                GdkAtom          selection,
                                                GdkAtom          target,
                                                GdkAtom          property,
                                                guint32          time)
{
  State *st = state_peek (display);
  NidEntry *e;

  if (st == NULL || requestor == 0)
    return;

  e = g_hash_table_lookup (st->ids, GUINT_TO_POINTER ((guint32) requestor));
  if (e == NULL)
    return;

  switch (e->kind)
    {
    case NID_WINDOW:
      {
        GdkWindow *window = e->window;
        GList *l;

        for (l = st->ops; l != NULL; l = l->next)
          {
            Op *op = l->data;

            if (op->kind == OP_LOCAL && op->requestor == window &&
                op->selection == selection && op->target == target)
              {
                guint32 when = time != 0 ? time : op->time;

                op_free (op, FALSE);
                put_notify (window, selection, target, property, when);
                break;
              }
          }
      }
      break;

    case NID_SEND:
      send_answer (e->op, property);
      break;

    case NID_PROBE:
      probe_answer (e->op, property);
      break;

    case NID_DISCARD:
      {
        Op *op = e->op;

        if (op->owner != NULL)
          _gdk_wayland_property_delete (op->owner, op->property);
        op_free (op, FALSE);
      }
      break;
    }
}

/* ---------------------------------------------------------------------------
 * Window destruction (integrator call site I4)
 * ------------------------------------------------------------------------ */

static void
state_window_destroyed (State *st, GdkWindow *window)
{
  GHashTableIter iter;
  gpointer key, value;
  GList *l, *copy;
  GList *gone = NULL;

  g_hash_table_iter_init (&iter, st->recs);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      SelRec *rec = value;

      if (rec->has_owner && rec->owner == window)
        rec_drop_owner (rec);
    }

  copy = g_list_copy (st->ops);
  for (l = copy; l != NULL; l = l->next)
    {
      Op *op = l->data;

      switch (op->kind)
        {
        case OP_LOCAL:
          if (op->requestor == window)
            op_free (op, FALSE);
          else if (op->owner == window)
            {
              /* the requestor is still owed its one NOTIFY */
              put_notify (op->requestor, op->selection, op->target, GDK_NONE,
                          op->time);
              op_free (op, FALSE);
            }
          break;
        case OP_RECV:
          if (op->requestor == window)
            op_free (op, TRUE);
          break;
        case OP_SEND:
        case OP_PROBE:
        case OP_DISCARD:
          if (op->owner == window)
            op_free (op, TRUE);
          break;
        }
    }
  g_list_free (copy);

  /* forget the native id of the window */
  g_hash_table_iter_init (&iter, st->ids);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      NidEntry *e = value;

      if (e->kind == NID_WINDOW && e->window == window)
        gone = g_list_prepend (gone, key);
    }
  for (l = gone; l != NULL; l = l->next)
    g_hash_table_remove (st->ids, l->data);
  g_list_free (gone);
}

void
_gdk_wayland_selection_window_destroyed (GdkWindow *window)
{
  GList *l;

  if (window == NULL || !GDK_IS_WINDOW (window))
    return;

  for (l = all_states; l != NULL; l = l->next)
    state_window_destroyed (l->data, window);

  g_object_set_data (G_OBJECT (window), NATIVE_ID_KEY, NULL);

  _gdk_wayland_property_window_destroyed (window);
  _gdk_wayland_dnd_window_destroyed (window);
}

/* ---------------------------------------------------------------------------
 * Test hooks
 * ------------------------------------------------------------------------ */

static void
fill_offer_info (GdkDisplay *display, GdkAtom selection, GDKWTestSelectionInfo *info)
{
  GDKWDataDevice *dd = _gdk_wayland_display_get_data_device (display);
  GDKWSelKind kind = _gdk_wayland_selection_kind (selection);
  GDKWOffer *offer = NULL;

  if (kind == GDKW_SEL_CLIPBOARD || kind == GDKW_SEL_PRIMARY)
    offer = dd->selection_offer[kind];
  else if (kind == GDKW_SEL_DND)
    offer = dd->dnd.dest_offer;

  info->external_offer = offer != NULL && !offer->dead;
  info->external_offer_id = info->external_offer ? offer->id : 0;
}

gboolean
_gdk_wayland_test_selection_info (GdkDisplay            *display,
                                  GdkAtom                selection,
                                  GDKWTestSelectionInfo *info)
{
  State *st;
  SelRec *rec;
  GList *l;

  if (display == NULL || !GDK_IS_DISPLAY (display) ||
      !GDKW_IS_DISPLAY_WAYLAND (display) || info == NULL)
    return FALSE;

  memset (info, 0, sizeof (*info));
  st = state_peek (display);
  rec = (st != NULL && selection != GDK_NONE) ? rec_find (st, selection) : NULL;
  fill_offer_info (display, selection, info);
  if (rec == NULL)
    return TRUE;

  info->has_owner = rec->has_owner && window_alive (rec->owner);
  info->owner = info->has_owner ? rec->owner : NULL;
  info->owner_time = rec->time;
  info->generation = rec->generation;
  info->source_alive = rec->source != NULL;
  info->probing = rec->probe != NULL;
  info->probes_started = rec->probes_started;
  info->probes_completed = rec->probes_completed;
  info->clear_events = rec->clear_events;

  for (l = st->ops; l != NULL; l = l->next)
    {
      Op *op = l->data;

      if (op->selection != selection)
        continue;
      if (op->kind == OP_LOCAL)
        info->local_pending++;
      else if (op->kind == OP_SEND)
        info->send_pending++;
    }

  return TRUE;
}

/* Split @text (NUL separated, @length bytes) into a NULL terminated vector
 * of strings, converting each from @from_charset to @to_charset.  Strings
 * that fail to convert are dropped.  Returns the number of strings. */
static gint
make_list (const gchar  *text,
	   gint          length,
	   const gchar  *from_charset,
	   const gchar  *to_charset,
	   gchar      ***list)
{
  GPtrArray *strings = g_ptr_array_new_with_free_func (g_free);
  const gchar *p = text;
  gint n_strings;

  while (p < text + length)
    {
      const gchar *q = p;
      gchar *str;

      while (q < text + length && *q)
	q++;

      if (strcmp (from_charset, to_charset) == 0)
	{
	  str = g_strndup (p, q - p);
	  if (strcmp (to_charset, "UTF-8") == 0 &&
	      !g_utf8_validate (str, -1, NULL))
	    {
	      g_warning ("Error converting selection from UTF8_STRING");
	      g_free (str);
	      str = NULL;
	    }
	}
      else
	{
	  GError *error = NULL;

	  str = g_convert (p, q - p, to_charset, from_charset,
			   NULL, NULL, &error);
	  if (!str)
	    {
	      g_warning ("Error converting selection from '%s' to '%s': %s",
			 from_charset, to_charset, error->message);
	      g_error_free (error);
	    }
	}

      if (str)
	g_ptr_array_add (strings, str);

      p = q + 1;
    }

  n_strings = strings->len;

  if (list)
    {
      g_ptr_array_add (strings, NULL);
      *list = (gchar **) g_ptr_array_free (strings, FALSE);
    }
  else
    g_ptr_array_free (strings, TRUE);

  return n_strings;
}

/* Returns the source charset for @encoding, or NULL if unsupported. */
static const gchar *
encoding_charset (GdkAtom encoding)
{
  if (encoding == GDK_TARGET_STRING)
    return "ISO-8859-1";
  else if (encoding == gdk_atom_intern_static_string ("UTF8_STRING"))
    return "UTF-8";

  return NULL;
}

gint
_gdk_wayland_text_property_to_text_list_for_display (GdkDisplay   *display,
						     GdkAtom       encoding,
						     gint          format,
						     const guchar *text,
						     gint          length,
						     gchar      ***list)
{
  const gchar *from_charset;
  const gchar *locale_charset = NULL;

  g_return_val_if_fail (GDK_IS_DISPLAY (display), 0);

  if (list)
    *list = NULL;

  /* COMPOUND_TEXT and other X encodings are not supported on Wayland. */
  from_charset = encoding_charset (encoding);
  if (!from_charset || !text || length <= 0)
    return 0;

  g_get_charset (&locale_charset);

  return make_list ((const gchar *) text, length,
		    from_charset, locale_charset, list);
}

gint
_gdk_wayland_text_property_to_utf8_list_for_display (GdkDisplay    *display,
						     GdkAtom        encoding,
						     gint           format,
						     const guchar  *text,
						     gint           length,
						     gchar       ***list)
{
  const gchar *from_charset;

  g_return_val_if_fail (text != NULL, 0);
  g_return_val_if_fail (length >= 0, 0);
  g_return_val_if_fail (GDK_IS_DISPLAY (display), 0);

  from_charset = encoding_charset (encoding);
  if (!from_charset)
    {
      if (list)
	*list = NULL;
      return 0;
    }

  return make_list ((const gchar *) text, length,
		    from_charset, "UTF-8", list);
}

void
_gdk_wayland_free_text_list (gchar **list)
{
  g_return_if_fail (list != NULL);

  g_strfreev (list);
}

void
_gdk_wayland_free_compound_text (guchar *ctext)
{
  g_free (ctext);
}

/* Strip C0/C1 controls other than \n and \t and canonicalize \r and \r\n
 * to \n, as the X11 backend does before producing text properties. */
static gchar *
sanitize_utf8 (const gchar *src)
{
  GString *result = g_string_sized_new (strlen (src));
  const gchar *p = src;

  while (*p)
    {
      if (*p == '\r')
	{
	  p++;
	  if (*p == '\n')
	    p++;

	  g_string_append_c (result, '\n');
	}
      else
	{
	  gunichar ch = g_utf8_get_char (p);

	  if (!((ch < 0x20 && ch != '\t' && ch != '\n') ||
		(ch >= 0x7f && ch < 0xa0)))
	    g_string_append_unichar (result, ch);

	  p = g_utf8_next_char (p);
	}
    }

  return g_string_free (result, FALSE);
}

static void
clear_ctext_out (GdkAtom *encoding,
		 gint    *format,
		 guchar **ctext,
		 gint    *length)
{
  if (encoding)
    *encoding = GDK_NONE;
  if (format)
    *format = 0;
  if (ctext)
    *ctext = NULL;
  if (length)
    *length = 0;
}

/* "Compound text" on Wayland is plain UTF-8 tagged UTF8_STRING: that is
 * what Wayland clients exchange (text/plain;charset=utf-8).  Returns 0 on
 * success, non-zero on failure, matching the X11 contract. */
gint
_gdk_wayland_string_to_compound_text_for_display (GdkDisplay  *display,
						  const gchar *str,
						  GdkAtom     *encoding,
						  gint        *format,
						  guchar     **ctext,
						  gint        *length)
{
  const gchar *charset = NULL;
  gchar *utf8, *clean;

  g_return_val_if_fail (GDK_IS_DISPLAY (display), 1);
  g_return_val_if_fail (str != NULL, 1);

  if (g_get_charset (&charset))
    utf8 = g_strdup (str);
  else
    utf8 = g_convert (str, -1, "UTF-8", charset, NULL, NULL, NULL);

  if (!utf8 || !g_utf8_validate (utf8, -1, NULL))
    {
      g_free (utf8);
      clear_ctext_out (encoding, format, ctext, length);
      return 1;
    }

  clean = sanitize_utf8 (utf8);
  g_free (utf8);

  if (encoding)
    *encoding = gdk_atom_intern_static_string ("UTF8_STRING");
  if (format)
    *format = 8;
  if (length)
    *length = strlen (clean);
  if (ctext)
    *ctext = (guchar *) clean;
  else
    g_free (clean);

  return 0;
}

gboolean
_gdk_wayland_utf8_to_compound_text_for_display (GdkDisplay  *display,
						const gchar *str,
						GdkAtom     *encoding,
						gint        *format,
						guchar     **ctext,
						gint        *length)
{
  gchar *clean;

  g_return_val_if_fail (str != NULL, FALSE);
  g_return_val_if_fail (GDK_IS_DISPLAY (display), FALSE);

  /* sanitize_utf8 steps through the string by character, so it must only
   * ever see valid UTF-8 (a truncated sequence would run past the NUL). */
  if (!g_utf8_validate (str, -1, NULL))
    {
      clear_ctext_out (encoding, format, ctext, length);
      return FALSE;
    }

  clean = sanitize_utf8 (str);

  if (encoding)
    *encoding = gdk_atom_intern_static_string ("UTF8_STRING");
  if (format)
    *format = 8;
  if (length)
    *length = strlen (clean);
  if (ctext)
    *ctext = (guchar *) clean;
  else
    g_free (clean);

  return TRUE;
}
