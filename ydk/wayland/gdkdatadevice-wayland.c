/*
 * gdkdatadevice-wayland.c
 *
 * ydk-wayland fork, M8 (selection, clipboard, drag and drop): the data
 * device layer.  This is the ONLY file that calls wl_data_* and
 * zwp_primary_selection_* requests.  It owns:
 *
 *   - registry binding of wl_data_device_manager and the primary selection
 *     manager, one device of each per seat (the first seat only);
 *   - GDKWOffer and GDKWSource records and the single listener tables, for
 *     CLIPBOARD, PRIMARY (same state machine, other wire names) and DnD;
 *   - the asynchronous pipe reader and writer (non blocking, on the default
 *     main context, idle timeouts, SIGPIPE safe);
 *   - the virtual device and all the event injection hooks of the headless
 *     tests (the demo drives the REAL listener functions with fake proxies).
 *
 * The contract is ydk/wayland/ydk/gdkm8-wayland.h (DECISIONS: PIPES,
 * TIMEOUTS, LIMITS, PRIMARY and HOOK OWNERS).
 *
 * Reference rules used in this file
 *   - A GDKWOffer is shared by reference count.  The device holds one
 *     reference while the offer is in dd->offers (until it is destroyed).
 *     dd->selection_offer[] holds NO reference of its own.  dd->dnd.dest_offer
 *     is the dnd piece's reference: _gdk_wayland_dd_offer_destroy () clears
 *     the slot but does not unref it.
 *   - A pipe op owns its fd and its GSources, and holds one reference on the
 *     offer it reads from.  The completion callback runs from the main loop
 *     after the op is already freed.
 *   - In virtual mode there are no proxies at all (every proxy pointer is
 *     NULL) and every request is recorded in dd->rec instead of sent.
 */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>
#include <glib-unix.h>

#include <wayland-client.h>

#include <ydk/gdk.h>
#include "gdkprivate-wayland.h"
#include "gdkseat-wayland.h"
#include "gdkm8-wayland.h"
#include "primary-selection-unstable-v1-client.h"

#ifdef GDK_COMPILATION
#include "ydk/gdkalias.h"
#endif

#define GDKW_FEED_CHUNK 4096

/* ------------------------------------------------------------------------ */
/* Private types                                                            */
/* ------------------------------------------------------------------------ */

struct _GDKWPipeOp
{
  GDKWDataDevice *dd;
  gboolean        is_write;
  gboolean        internal;     /* the pretend sender: not counted as a pending write */
  int             fd;           /* -1 once closed */
  GSource        *fd_source;
  GSource        *timeout_source;
  GSource        *idle_source;  /* deferred failure */
  guint           timeout_ms;
  GDKWPipeOp     *peer;         /* reader <-> pretend sender */
  gpointer        user_data;

  /* reader */
  GDKWOffer      *offer;        /* reference held */
  guchar         *buf;
  gsize           len;
  gsize           cap;
  GDKWReadDone    read_done;

  /* writer */
  GBytes         *bytes;
  gsize           off;
  gsize           chunk;        /* 0: GDKW_PIPE_CHUNK per dispatch */
  GDKWWriteDone   write_done;
};

typedef struct
{
  GBytes      *bytes;
  GDKWTestFeed feed;
} GDKWFeed;

/* The offer handed to the listeners by a test hook (virtual mode has no
 * proxy to carry it). */
static GDKWOffer *test_offer_arg = NULL;

/* ------------------------------------------------------------------------ */
/* Small helpers                                                            */
/* ------------------------------------------------------------------------ */

static void
copy_mime (gchar *dest, const gchar *src)
{
  if (src == NULL)
    dest[0] = '\0';
  else
    g_strlcpy (dest, src, GDKW_TEST_MIME_LEN);
}

static void
dd_flush (GDKWDataDevice *dd)
{
  if (dd->virtual_device || dd->display == NULL ||
      dd->display->wl_display == NULL)
    return;

  /* EAGAIN means the socket is full; the event loop flushes later. */
  wl_display_flush (dd->display->wl_display);
}

static GdkDisplay *
dd_gdk_display (GDKWDataDevice *dd)
{
  return dd->display ? GDK_DISPLAY_OBJECT (dd->display) : NULL;
}

static GDKWDataDevice *
dd_peek (GdkDisplay *display)
{
  if (display == NULL || !GDKW_IS_DISPLAY_WAYLAND (display))
    return NULL;

  return g_object_get_data (G_OBJECT (display), GDKW_DATADEVICE_DATA_KEY);
}

static GDKWDataDevice *
dd_get (GdkDisplay *display)
{
  if (display == NULL || !GDKW_IS_DISPLAY_WAYLAND (display))
    return NULL;

  return _gdk_wayland_display_get_data_device (display);
}

static GDKWDataDevice *
dd_get_virtual (GdkDisplay *display)
{
  GDKWDataDevice *dd = dd_get (display);

  return (dd != NULL && dd->virtual_device && !dd->finalized) ? dd : NULL;
}

static void
ensure_sigpipe_ignored (void)
{
  static gboolean done = FALSE;
  struct sigaction old_act;
  struct sigaction new_act;

  if (done)
    return;
  done = TRUE;

  if (sigaction (SIGPIPE, NULL, &old_act) != 0)
    return;

  if (!(old_act.sa_flags & SA_SIGINFO) && old_act.sa_handler == SIG_DFL)
    {
      memset (&new_act, 0, sizeof new_act);
      new_act.sa_handler = SIG_IGN;
      sigemptyset (&new_act.sa_mask);
      sigaction (SIGPIPE, &new_act, NULL);
    }
}

static GDKWFeed *
feed_new (const guint8 *data, gsize length, GDKWTestFeed feed)
{
  GDKWFeed *f = g_new0 (GDKWFeed, 1);

  f->bytes = g_bytes_new (data, length);
  f->feed = feed;
  return f;
}

static void
feed_free (gpointer data)
{
  GDKWFeed *f = data;

  g_bytes_unref (f->bytes);
  g_free (f);
}

static gchar *
feed_key (guint offer_id, const gchar *mime)
{
  return g_strdup_printf ("%u:%s", offer_id, mime);
}

/* ------------------------------------------------------------------------ */
/* Pipe operations                                                          */
/* ------------------------------------------------------------------------ */

static void
op_close_fd (GDKWPipeOp *op)
{
  if (op->fd >= 0)
    {
      close (op->fd);
      op->fd = -1;
    }
}

static void
op_clear_source (GSource **source)
{
  if (*source != NULL)
    {
      g_source_destroy (*source);
      g_source_unref (*source);
      *source = NULL;
    }
}

/* Frees everything of @op without calling its completion.  A reader also
 * releases its pretend sender. */
static void
op_release (GDKWPipeOp *op)
{
  GDKWDataDevice *dd = op->dd;
  GDKWPipeOp *peer = op->peer;

  op->peer = NULL;
  if (peer != NULL)
    {
      peer->peer = NULL;
      if (!op->is_write)
        op_release (peer);
    }

  op_clear_source (&op->fd_source);
  op_clear_source (&op->timeout_source);
  op_clear_source (&op->idle_source);
  op_close_fd (op);

  if (dd != NULL)
    {
      dd->ops = g_list_remove (dd->ops, op);
      if (!op->is_write && dd->rec.pending_reads > 0)
        dd->rec.pending_reads--;
      else if (op->is_write && !op->internal && dd->rec.pending_writes > 0)
        dd->rec.pending_writes--;
    }

  if (op->offer != NULL)
    _gdk_wayland_dd_offer_unref (op->offer);
  g_free (op->buf);
  if (op->bytes != NULL)
    g_bytes_unref (op->bytes);
  g_free (op);
}

static void
op_complete_read (GDKWPipeOp *op, GDKWReadStatus status)
{
  GDKWReadDone done = op->read_done;
  gpointer user_data = op->user_data;
  guchar *data = NULL;
  gsize length = 0;

  if (status == GDKW_READ_OK)
    {
      length = op->len;
      data = g_realloc (op->buf, length + 1);
      data[length] = '\0';
      op->buf = NULL;
    }

  op_release (op);

  if (done != NULL)
    done (status, data, length, user_data);
  else
    g_free (data);
}

static void
op_complete_write (GDKWPipeOp *op, gboolean ok)
{
  GDKWWriteDone done = op->write_done;
  gpointer user_data = op->user_data;

  op_release (op);

  if (done != NULL)
    done (ok, user_data);
}

static gboolean
op_timeout_cb (gpointer data)
{
  GDKWPipeOp *op = data;

  if (op->is_write)
    op_complete_write (op, FALSE);
  else
    op_complete_read (op, GDKW_READ_TIMEOUT);

  return G_SOURCE_REMOVE;
}

/* (Re)arms the idle timeout; called at creation and after every progress. */
static void
op_arm_timeout (GDKWPipeOp *op)
{
  op_clear_source (&op->timeout_source);

  if (op->timeout_ms == 0)
    return;

  op->timeout_source = g_timeout_source_new (op->timeout_ms);
  g_source_set_callback (op->timeout_source, op_timeout_cb, op, NULL);
  g_source_attach (op->timeout_source, NULL);
}

static gboolean
read_cb (gint fd, GIOCondition condition, gpointer data)
{
  GDKWPipeOp *op = data;
  gsize want;
  gssize n;

  want = MIN ((gsize) GDKW_PIPE_CHUNK,
              (gsize) GDKW_SELECTION_MAX_BYTES + 1 - op->len);
  if (op->cap - op->len < want)
    {
      op->cap = MAX (op->cap * 2, op->len + want);
      op->buf = g_realloc (op->buf, op->cap);
    }

  do
    n = read (fd, op->buf + op->len, want);
  while (n < 0 && errno == EINTR);

  if (n > 0)
    {
      op->len += (gsize) n;
      if (op->len > (gsize) GDKW_SELECTION_MAX_BYTES)
        {
          op_complete_read (op, GDKW_READ_TOO_BIG);
          return G_SOURCE_REMOVE;
        }
      op_arm_timeout (op);
      return G_SOURCE_CONTINUE;
    }

  if (n == 0)
    {
      op_complete_read (op, GDKW_READ_OK);
      return G_SOURCE_REMOVE;
    }

  if (errno == EAGAIN || errno == EWOULDBLOCK)
    return G_SOURCE_CONTINUE;

  op_complete_read (op, GDKW_READ_ERROR);
  return G_SOURCE_REMOVE;
}

static gboolean
write_cb (gint fd, GIOCondition condition, gpointer data)
{
  GDKWPipeOp *op = data;
  gsize size = g_bytes_get_size (op->bytes);
  gsize remaining = size - op->off;
  gsize chunk;
  gssize n;

  if (remaining == 0)
    {
      op_complete_write (op, TRUE);
      return G_SOURCE_REMOVE;
    }

  chunk = op->chunk != 0 ? MIN (op->chunk, remaining)
                         : MIN ((gsize) GDKW_PIPE_CHUNK, remaining);

  do
    n = write (fd, (const guchar *) g_bytes_get_data (op->bytes, NULL) + op->off,
               chunk);
  while (n < 0 && errno == EINTR);

  if (n > 0)
    {
      op->off += (gsize) n;
      if (op->off >= size)
        {
          op_complete_write (op, TRUE);
          return G_SOURCE_REMOVE;
        }
      op_arm_timeout (op);
      return G_SOURCE_CONTINUE;
    }

  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    return G_SOURCE_CONTINUE;

  /* EPIPE (reader gone) or any other error */
  op_complete_write (op, FALSE);
  return G_SOURCE_REMOVE;
}

static gboolean
write_fail_idle_cb (gpointer data)
{
  GDKWPipeOp *op = data;

  op_complete_write (op, FALSE);
  return G_SOURCE_REMOVE;
}

static GDKWPipeOp *
op_new (GDKWDataDevice *dd, gboolean is_write, int fd, guint timeout_ms)
{
  GDKWPipeOp *op = g_new0 (GDKWPipeOp, 1);

  op->dd = dd;
  op->is_write = is_write;
  op->fd = fd;
  op->timeout_ms = timeout_ms;
  if (dd != NULL)
    dd->ops = g_list_prepend (dd->ops, op);
  return op;
}

static void
op_attach_fd_source (GDKWPipeOp *op, GIOCondition condition, GUnixFDSourceFunc func)
{
  op->fd_source = g_unix_fd_source_new (op->fd, condition);
  g_source_set_callback (op->fd_source, (GSourceFunc) func, op, NULL);
  g_source_attach (op->fd_source, NULL);
}

/* Shared by _gdk_wayland_dd_write_async () and the pretend sender. */
static GDKWPipeOp *
write_op_new (GDKWDataDevice *dd,
              int             fd,
              GBytes         *bytes,
              guint           timeout_ms,
              gsize           chunk,
              gboolean        internal,
              GDKWWriteDone   done,
              gpointer        user_data)
{
  GDKWPipeOp *op;

  ensure_sigpipe_ignored ();

  op = op_new (dd, TRUE, fd,
                timeout_ms != 0 ? timeout_ms
                : (dd != NULL ? dd->send_timeout_ms : GDKW_SEND_TIMEOUT_MS));
  op->internal = internal;
  op->bytes = bytes != NULL ? g_bytes_ref (bytes) : g_bytes_new (NULL, 0);
  op->chunk = chunk;
  op->write_done = done;
  op->user_data = user_data;
  if (!internal && dd != NULL)
    dd->rec.pending_writes++;

  if (fd < 0 ||
      fcntl (fd, F_SETFD, FD_CLOEXEC) < 0 ||
      !g_unix_set_fd_nonblocking (fd, TRUE, NULL))
    {
      /* unusable fd: report the failure from the main loop */
      op->idle_source = g_idle_source_new ();
      g_source_set_callback (op->idle_source, write_fail_idle_cb, op, NULL);
      g_source_attach (op->idle_source, NULL);
      return op;
    }

  op_attach_fd_source (op, G_IO_OUT | G_IO_ERR | G_IO_HUP, write_cb);
  op_arm_timeout (op);
  return op;
}

GDKWPipeOp *
_gdk_wayland_dd_write_async (int            fd,
                             GBytes        *bytes,
                             guint          timeout_ms,
                             GDKWWriteDone  done,
                             gpointer       user_data)
{
  GdkDisplay *display = gdk_display_get_default ();
  GDKWDataDevice *dd;

  dd = dd_get (display);
  if (dd != NULL && dd->finalized)
    dd = NULL;           /* untracked op: nothing to gauge, still async */

  return write_op_new (dd, fd, bytes, timeout_ms, 0, FALSE, done, user_data);
}

void
_gdk_wayland_dd_pipe_cancel (GDKWPipeOp *op)
{
  if (op != NULL)
    op_release (op);
}

/* ------------------------------------------------------------------------ */
/* Offers                                                                   */
/* ------------------------------------------------------------------------ */

static GDKWOffer *
offer_create (GDKWDataDevice *dd,
              GDKWSelKind     kind,
              struct wl_data_offer *wl_offer,
              struct zwp_primary_selection_offer_v1 *zwp_offer)
{
  GDKWOffer *offer = g_new0 (GDKWOffer, 1);

  offer->dd = dd;
  offer->id = dd->next_offer_id++;
  if (dd->next_offer_id == 0)
    dd->next_offer_id = 1;
  offer->ref_count = 1;                 /* the device's reference */
  offer->kind = kind;
  offer->wl_offer = wl_offer;
  offer->zwp_offer = zwp_offer;
  offer->version = 1;
  if (wl_offer != NULL)
    offer->version = wl_data_offer_get_version (wl_offer);
  offer->mimes = g_ptr_array_new_with_free_func (g_free);

  dd->offers = g_list_prepend (dd->offers, offer);
  dd->rec.offers_alive++;
  return offer;
}

static GDKWOffer *
offer_find (GDKWDataDevice *dd, guint id)
{
  GList *l;

  for (l = dd->offers; l; l = l->next)
    {
      GDKWOffer *offer = l->data;

      if (offer->id == id)
        return offer;
    }

  return NULL;
}

GDKWOffer *
_gdk_wayland_dd_offer_ref (GDKWOffer *offer)
{
  if (offer != NULL)
    offer->ref_count++;
  return offer;
}

static void
offer_destroy_proxy (GDKWOffer *offer)
{
  if (offer->wl_offer != NULL)
    {
      wl_data_offer_destroy (offer->wl_offer);
      offer->wl_offer = NULL;
    }
  if (offer->zwp_offer != NULL)
    {
      zwp_primary_selection_offer_v1_destroy (offer->zwp_offer);
      offer->zwp_offer = NULL;
    }
}

static void
feeds_remove_offer (GDKWDataDevice *dd, guint offer_id)
{
  GHashTableIter iter;
  gpointer key;
  gchar *prefix;

  if (dd->feeds == NULL)
    return;

  prefix = g_strdup_printf ("%u:", offer_id);
  g_hash_table_iter_init (&iter, dd->feeds);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      if (g_str_has_prefix (key, prefix))
        g_hash_table_iter_remove (&iter);
    }
  g_free (prefix);
}

void
_gdk_wayland_dd_offer_unref (GDKWOffer *offer)
{
  GDKWDataDevice *dd;

  if (offer == NULL)
    return;

  g_return_if_fail (offer->ref_count > 0);

  if (--offer->ref_count > 0)
    return;

  dd = offer->dd;
  if (dd != NULL)
    {
      if (offer->wl_offer != NULL || offer->zwp_offer != NULL)
        offer_destroy_proxy (offer);
      dd->offers = g_list_remove (dd->offers, offer);
      if (dd->rec.offers_alive > 0)
        dd->rec.offers_alive--;
      feeds_remove_offer (dd, offer->id);
    }

  g_ptr_array_free (offer->mimes, TRUE);
  g_free (offer);
}

/* Marks @offer dead, destroys its proxy, clears the device slots and drops the
 * device's reference.  @count_it records the destroy request for the tests. */
static void
offer_kill (GDKWOffer *offer, gboolean destroy_proxy, gboolean count_it)
{
  GDKWDataDevice *dd = offer->dd;

  if (offer->dead)
    return;
  offer->dead = TRUE;

  if (destroy_proxy)
    offer_destroy_proxy (offer);
  else
    {
      offer->wl_offer = NULL;
      offer->zwp_offer = NULL;
    }

  if (dd == NULL)
    return;

  if (count_it)
    dd->rec.offer_destroy_count++;

  if (dd->selection_offer[GDKW_SEL_CLIPBOARD] == offer)
    dd->selection_offer[GDKW_SEL_CLIPBOARD] = NULL;
  if (dd->selection_offer[GDKW_SEL_PRIMARY] == offer)
    dd->selection_offer[GDKW_SEL_PRIMARY] = NULL;
  if (dd->dnd.dest_offer == offer)
    dd->dnd.dest_offer = NULL;       /* the slot's reference stays with its owner */

  if (g_list_find (dd->offers, offer) != NULL)
    {
      dd->offers = g_list_remove (dd->offers, offer);
      _gdk_wayland_dd_offer_unref (offer);     /* the device's reference */
    }
}

void
_gdk_wayland_dd_offer_destroy (GDKWOffer *offer)
{
  if (offer == NULL || offer->dead)
    return;

  GDKWDataDevice *dd = offer->dd;

  _gdk_wayland_dd_offer_ref (offer);       /* survive our own unref */
  offer_kill (offer, TRUE, TRUE);
  if (dd != NULL)
    dd_flush (dd);
  _gdk_wayland_dd_offer_unref (offer);
}

void
_gdk_wayland_dd_offer_accept (GDKWOffer   *offer,
                              guint32      serial,
                              const gchar *mime)
{
  GDKWDataDevice *dd;

  if (offer == NULL || offer->dead || offer->dd == NULL)
    return;
  dd = offer->dd;

  if (dd->virtual_device)
    {
      if (offer->kind == GDKW_SEL_PRIMARY)
        return;
    }
  else if (offer->wl_offer == NULL)
    return;

  dd->rec.accept_count++;
  dd->rec.accept_serial = serial;
  dd->rec.accept_null = (mime == NULL);
  copy_mime (dd->rec.accept_mime, mime);

  if (!dd->virtual_device)
    {
      wl_data_offer_accept (offer->wl_offer, serial, mime);
      dd_flush (dd);
    }
}

void
_gdk_wayland_dd_offer_set_actions (GDKWOffer *offer,
                                   guint32    wl_mask,
                                   guint32    wl_preferred)
{
  GDKWDataDevice *dd;

  if (offer == NULL || offer->dead || offer->dd == NULL ||
      offer->version < 3 || offer->kind == GDKW_SEL_PRIMARY)
    return;
  dd = offer->dd;

  if (!dd->virtual_device && offer->wl_offer == NULL)
    return;

  dd->rec.set_actions_count++;
  dd->rec.offer_actions_mask = wl_mask;
  dd->rec.offer_actions_preferred = wl_preferred;

  if (!dd->virtual_device)
    {
      wl_data_offer_set_actions (offer->wl_offer, wl_mask, wl_preferred);
      dd_flush (dd);
    }
}

void
_gdk_wayland_dd_offer_finish (GDKWOffer *offer)
{
  GDKWDataDevice *dd;

  if (offer == NULL || offer->dead || offer->dd == NULL ||
      offer->version < 3 || offer->kind == GDKW_SEL_PRIMARY)
    return;
  dd = offer->dd;

  if (!dd->virtual_device && offer->wl_offer == NULL)
    return;

  dd->rec.finish_count++;

  if (!dd->virtual_device)
    {
      wl_data_offer_finish (offer->wl_offer);
      dd_flush (dd);
    }
}

GDKWPipeOp *
_gdk_wayland_dd_offer_receive (GDKWOffer    *offer,
                               const gchar  *mime,
                               GDKWReadDone  done,
                               gpointer      user_data)
{
  GDKWDataDevice *dd;
  GDKWPipeOp *op;
  int fds[2];

  if (offer == NULL || offer->dead || offer->dd == NULL || mime == NULL)
    return NULL;
  dd = offer->dd;

  if (!dd->virtual_device && offer->wl_offer == NULL && offer->zwp_offer == NULL)
    return NULL;

  if (!g_unix_open_pipe (fds, O_CLOEXEC, NULL))
    return NULL;

  if (!g_unix_set_fd_nonblocking (fds[0], TRUE, NULL))
    {
      close (fds[0]);
      close (fds[1]);
      return NULL;
    }

  op = op_new (dd, FALSE, fds[0], dd->receive_timeout_ms);
  op->offer = _gdk_wayland_dd_offer_ref (offer);
  op->read_done = done;
  op->user_data = user_data;
  dd->rec.pending_reads++;

  dd->rec.receive_count++;
  dd->rec.receive_kind = offer->kind;
  copy_mime (dd->rec.receive_mime, mime);

  if (dd->virtual_device)
    {
      gchar *key = feed_key (offer->id, mime);
      GDKWFeed *feed = dd->feeds ? g_hash_table_lookup (dd->feeds, key) : NULL;

      g_free (key);

      if (feed != NULL && feed->feed == GDKW_TEST_FEED_NORMAL)
        {
          GDKWPipeOp *sender = write_op_new (dd, fds[1], feed->bytes, 0,
                                             GDKW_FEED_CHUNK, TRUE, NULL, NULL);

          op->peer = sender;
          sender->peer = op;
        }
      else if (feed != NULL && feed->feed == GDKW_TEST_FEED_STALL)
        {
          /* hold the write end open until the reader is done */
          GDKWPipeOp *sender = op_new (dd, TRUE, fds[1], 0);

          sender->internal = TRUE;
          sender->bytes = g_bytes_new (NULL, 0);
          op->peer = sender;
          sender->peer = op;
        }
      else
        close (fds[1]);               /* ERROR feed or no program: immediate EOF */
    }
  else
    {
      if (offer->wl_offer != NULL)
        wl_data_offer_receive (offer->wl_offer, mime, fds[1]);
      else
        zwp_primary_selection_offer_v1_receive (offer->zwp_offer, mime, fds[1]);

      /* libwayland has dup'ed the fd into the message */
      close (fds[1]);
      dd_flush (dd);
    }

  op_attach_fd_source (op, G_IO_IN | G_IO_HUP | G_IO_ERR, read_cb);
  op_arm_timeout (op);
  return op;
}

/* ------------------------------------------------------------------------ */
/* Offer listeners (wl_data_offer and the primary selection offer)          */
/* ------------------------------------------------------------------------ */

static void
offer_add_mime (GDKWOffer *offer, const char *mime)
{
  if (offer == NULL || mime == NULL)
    return;

  g_ptr_array_add (offer->mimes, g_strdup (mime));
}

static void
wl_offer_offer (void *data, struct wl_data_offer *proxy, const char *mime)
{
  offer_add_mime (data, mime);
}

static void
wl_offer_source_actions (void *data, struct wl_data_offer *proxy, uint32_t actions)
{
  GDKWOffer *offer = data;
  GDKWDataDevice *dd = offer->dd;

  offer->source_actions = actions;
  offer->got_source_actions = TRUE;

  if (dd != NULL && !offer->dead && dd->dnd.dest_offer == offer)
    _gdk_wayland_dnd_offer_changed (dd, offer);
}

static void
wl_offer_action (void *data, struct wl_data_offer *proxy, uint32_t action)
{
  GDKWOffer *offer = data;
  GDKWDataDevice *dd = offer->dd;

  offer->action = action;
  offer->got_action = TRUE;

  if (dd != NULL && !offer->dead && dd->dnd.dest_offer == offer)
    _gdk_wayland_dnd_offer_changed (dd, offer);
}

static const struct wl_data_offer_listener wl_offer_listener = {
  .offer = wl_offer_offer,
  .source_actions = wl_offer_source_actions,
  .action = wl_offer_action
};

static void
zwp_offer_offer (void *data,
                 struct zwp_primary_selection_offer_v1 *proxy,
                 const char *mime)
{
  offer_add_mime (data, mime);
}

static const struct zwp_primary_selection_offer_v1_listener zwp_offer_listener = {
  .offer = zwp_offer_offer
};

/* ------------------------------------------------------------------------ */
/* Sources                                                                  */
/* ------------------------------------------------------------------------ */

static void
source_destroy_proxy (GDKWSource *src)
{
  if (src->wl_source != NULL)
    {
      wl_data_source_destroy (src->wl_source);
      src->wl_source = NULL;
    }
  if (src->zwp_source != NULL)
    {
      zwp_primary_selection_source_v1_destroy (src->zwp_source);
      src->zwp_source = NULL;
    }
}

/* Takes @src out of the device and frees it.  Common tail of _source_free ()
 * and the forced drop at teardown. */
static void
source_drop (GDKWSource *src, gboolean destroy_proxy)
{
  GDKWDataDevice *dd = src->dd;

  src->handlers = NULL;
  src->user_data = NULL;

  if (destroy_proxy)
    source_destroy_proxy (src);
  else
    {
      src->wl_source = NULL;
      src->zwp_source = NULL;
    }
  src->freed = TRUE;

  if (dd != NULL)
    {
      if (destroy_proxy)
        dd->rec.source_destroy_count++;
      dd->sources = g_list_remove (dd->sources, src);
      if (src->kind != GDKW_SEL_NONE && dd->source[src->kind] == src)
        dd->source[src->kind] = NULL;
      if (dd->rec.sources_alive > 0)
        dd->rec.sources_alive--;
    }

  g_ptr_array_free (src->mimes, TRUE);
  g_free (src);
}

void
_gdk_wayland_dd_source_free (GDKWSource *src)
{
  GDKWDataDevice *dd;

  if (src == NULL)
    return;
  dd = src->dd;

  source_drop (src, TRUE);
  dd_flush (dd);
}

static void
wl_source_target (void *data, struct wl_data_source *proxy, const char *mime)
{
  GDKWSource *src = data;

  if (!src->cancelled && src->handlers != NULL && src->handlers->target != NULL)
    src->handlers->target (src, mime);
}

static void
source_do_send (GDKWSource *src, const char *mime, int fd)
{
  if (src->cancelled || src->freed || src->handlers == NULL ||
      src->handlers->send == NULL || mime == NULL)
    {
      if (fd >= 0)
        close (fd);
      return;
    }

  src->handlers->send (src, mime, fd);
}

static void
wl_source_send (void *data, struct wl_data_source *proxy,
                const char *mime, int32_t fd)
{
  source_do_send (data, mime, fd);
}

static void
source_do_cancelled (GDKWSource *src)
{
  if (src->cancelled)
    return;
  src->cancelled = TRUE;

  /* the handler may free @src; touch nothing afterwards */
  if (src->handlers != NULL && src->handlers->cancelled != NULL)
    src->handlers->cancelled (src);
}

static void
wl_source_cancelled (void *data, struct wl_data_source *proxy)
{
  source_do_cancelled (data);
}

static void
wl_source_dnd_drop_performed (void *data, struct wl_data_source *proxy)
{
  GDKWSource *src = data;

  if (!src->cancelled && src->handlers != NULL &&
      src->handlers->dnd_drop_performed != NULL)
    src->handlers->dnd_drop_performed (src);
}

static void
wl_source_dnd_finished (void *data, struct wl_data_source *proxy)
{
  GDKWSource *src = data;

  if (!src->cancelled && src->handlers != NULL &&
      src->handlers->dnd_finished != NULL)
    src->handlers->dnd_finished (src);
}

static void
wl_source_action (void *data, struct wl_data_source *proxy, uint32_t action)
{
  GDKWSource *src = data;

  if (!src->cancelled && src->handlers != NULL && src->handlers->action != NULL)
    src->handlers->action (src, action);
}

static const struct wl_data_source_listener wl_source_listener = {
  .target = wl_source_target,
  .send = wl_source_send,
  .cancelled = wl_source_cancelled,
  .dnd_drop_performed = wl_source_dnd_drop_performed,
  .dnd_finished = wl_source_dnd_finished,
  .action = wl_source_action
};

static void
zwp_source_send (void *data,
                 struct zwp_primary_selection_source_v1 *proxy,
                 const char *mime, int32_t fd)
{
  source_do_send (data, mime, fd);
}

static void
zwp_source_cancelled (void *data, struct zwp_primary_selection_source_v1 *proxy)
{
  source_do_cancelled (data);
}

static const struct zwp_primary_selection_source_v1_listener zwp_source_listener = {
  .send = zwp_source_send,
  .cancelled = zwp_source_cancelled
};

GDKWSource *
_gdk_wayland_dd_source_new (GDKWDataDevice *dd,
                            GDKWSelKind     kind,
                            const gchar * const *mimes,
                            const GDKWSourceHandlers *handlers,
                            gpointer        user_data)
{
  GDKWSource *src;
  guint i;

  if (dd == NULL || dd->finalized || !_gdk_wayland_dd_usable (dd, kind))
    return NULL;

  src = g_new0 (GDKWSource, 1);
  src->dd = dd;
  src->id = dd->next_source_id++;
  if (dd->next_source_id == 0)
    dd->next_source_id = 1;
  src->kind = kind;
  src->handlers = handlers;
  src->user_data = user_data;
  src->mimes = g_ptr_array_new_with_free_func (g_free);

  for (i = 0; mimes != NULL && mimes[i] != NULL; i++)
    g_ptr_array_add (src->mimes, g_strdup (mimes[i]));

  if (!dd->virtual_device)
    {
      if (kind == GDKW_SEL_PRIMARY)
        {
          src->zwp_source =
            zwp_primary_selection_device_manager_v1_create_source (dd->primary_manager);
          if (src->zwp_source == NULL)
            goto fail;
          zwp_primary_selection_source_v1_add_listener (src->zwp_source,
                                                        &zwp_source_listener, src);
          for (i = 0; i < src->mimes->len; i++)
            zwp_primary_selection_source_v1_offer (src->zwp_source,
                                                   g_ptr_array_index (src->mimes, i));
        }
      else
        {
          src->wl_source = wl_data_device_manager_create_data_source (dd->manager);
          if (src->wl_source == NULL)
            goto fail;
          wl_data_source_add_listener (src->wl_source, &wl_source_listener, src);
          for (i = 0; i < src->mimes->len; i++)
            wl_data_source_offer (src->wl_source,
                                  g_ptr_array_index (src->mimes, i));
        }
      dd_flush (dd);
    }

  dd->sources = g_list_prepend (dd->sources, src);
  dd->source[kind] = src;
  dd->rec.sources_alive++;
  return src;

fail:
  g_ptr_array_free (src->mimes, TRUE);
  g_free (src);
  return NULL;
}

void
_gdk_wayland_dd_source_set_actions (GDKWSource *src, guint32 wl_mask)
{
  GDKWDataDevice *dd;

  if (src == NULL || src->freed || src->dd == NULL)
    return;
  dd = src->dd;

  if (dd->virtual_device)
    {
      if (dd->manager_version != 0 && dd->manager_version < 3)
        return;
    }
  else
    {
      if (src->wl_source == NULL || dd->manager_version < 3 ||
          wl_data_source_get_version (src->wl_source) < 3)
        return;
    }

  src->actions_mask = wl_mask;
  src->actions_set = TRUE;
  dd->rec.source_actions_set = TRUE;
  dd->rec.source_actions_mask = wl_mask;

  if (!dd->virtual_device)
    {
      wl_data_source_set_actions (src->wl_source, wl_mask);
      dd_flush (dd);
    }
}

void
_gdk_wayland_dd_set_selection (GDKWDataDevice *dd,
                               GDKWSelKind     kind,
                               GDKWSource     *src,
                               guint32         serial)
{
  guint i;
  gint index;

  if (dd == NULL || dd->finalized ||
      (kind != GDKW_SEL_CLIPBOARD && kind != GDKW_SEL_PRIMARY) ||
      !_gdk_wayland_dd_usable (dd, kind))
    return;

  if (src != NULL && (src->freed || src->kind != kind))
    src = NULL;

  index = (kind == GDKW_SEL_PRIMARY) ? 1 : 0;
  dd->rec.set_selection[index].count++;
  dd->rec.set_selection[index].serial = serial;
  dd->rec.set_selection[index].cleared = (src == NULL);
  if (src != NULL)
    {
      dd->rec.set_selection[index].n_mimes =
        MIN (src->mimes->len, (guint) GDKW_TEST_MAX_MIMES);
      for (i = 0; i < dd->rec.set_selection[index].n_mimes; i++)
        copy_mime (dd->rec.set_selection[index].mimes[i],
                   g_ptr_array_index (src->mimes, i));
    }

  if (!dd->virtual_device)
    {
      if (kind == GDKW_SEL_PRIMARY)
        zwp_primary_selection_device_v1_set_selection (dd->primary_device,
                                                       src ? src->zwp_source : NULL,
                                                       serial);
      else
        wl_data_device_set_selection (dd->device,
                                      src ? src->wl_source : NULL, serial);
      dd_flush (dd);
    }
}

gboolean
_gdk_wayland_dd_start_drag (GDKWDataDevice *dd,
                            GDKWSource     *src,
                            GdkWindow      *origin_toplevel,
                            guint32         serial)
{
  GDKWWindowImplWayland *impl;
  guint i;

  if (dd == NULL || dd->finalized || src == NULL || src->freed ||
      !_gdk_wayland_dd_usable (dd, GDKW_SEL_DND))
    return FALSE;

  impl = origin_toplevel ? _gdk_wayland_window_get_impl (origin_toplevel) : NULL;
  if (impl == NULL || impl->wl_surface == NULL)
    return FALSE;

  if (!dd->virtual_device && (dd->device == NULL || src->wl_source == NULL))
    return FALSE;

  dd->rec.start_drag_count++;
  dd->rec.start_drag_serial = serial;
  dd->rec.start_drag_origin = origin_toplevel;
  dd->rec.start_drag_icon = FALSE;
  dd->rec.start_drag_n_mimes = MIN (src->mimes->len, (guint) GDKW_TEST_MAX_MIMES);
  for (i = 0; i < dd->rec.start_drag_n_mimes; i++)
    copy_mime (dd->rec.start_drag_mimes[i], g_ptr_array_index (src->mimes, i));

  if (!dd->virtual_device)
    {
      wl_data_device_start_drag (dd->device, src->wl_source, impl->wl_surface,
                                 NULL, serial);
      dd_flush (dd);
    }

  return TRUE;
}

void
_gdk_wayland_dd_forget_self_offer (GDKWDataDevice *dd, GDKWSelKind kind)
{
  GDKWOffer *offer;

  if (dd == NULL || (kind != GDKW_SEL_CLIPBOARD && kind != GDKW_SEL_PRIMARY))
    return;

  offer = dd->selection_offer[kind];
  if (offer != NULL && offer->maybe_self)
    _gdk_wayland_dd_offer_destroy (offer);
}

/* ------------------------------------------------------------------------ */
/* Device listeners                                                         */
/* ------------------------------------------------------------------------ */

static GDKWOffer *
offer_from_wl (GDKWDataDevice *dd, struct wl_data_offer *proxy)
{
  if (dd->virtual_device)
    return test_offer_arg;
  if (proxy == NULL)
    return NULL;
  return wl_data_offer_get_user_data (proxy);
}

static GDKWOffer *
offer_from_zwp (GDKWDataDevice *dd, struct zwp_primary_selection_offer_v1 *proxy)
{
  if (dd->virtual_device)
    return test_offer_arg;
  if (proxy == NULL)
    return NULL;
  return zwp_primary_selection_offer_v1_get_user_data (proxy);
}

static void
dd_data_offer (void *data, struct wl_data_device *device, struct wl_data_offer *id)
{
  GDKWDataDevice *dd = data;
  GDKWOffer *offer;

  offer = offer_create (dd, GDKW_SEL_NONE, id, NULL);
  if (id != NULL)
    wl_data_offer_add_listener (id, &wl_offer_listener, offer);
}

/* A new current external offer for @kind (CLIPBOARD or PRIMARY), or none. */
static void
dd_set_current_offer (GDKWDataDevice *dd, GDKWSelKind kind, GDKWOffer *offer)
{
  GDKWOffer *old = dd->selection_offer[kind];

  if (old != NULL && old != offer)
    _gdk_wayland_dd_offer_destroy (old);

  dd->selection_offer[kind] = offer;
  if (offer == NULL)
    return;

  offer->kind = kind;
  offer->introduced = TRUE;
  offer->maybe_self =
    _gdk_wayland_selection_has_local_owner (dd_gdk_display (dd), kind);
}

static void
dd_selection (void *data, struct wl_data_device *device, struct wl_data_offer *id)
{
  GDKWDataDevice *dd = data;

  dd_set_current_offer (dd, GDKW_SEL_CLIPBOARD, offer_from_wl (dd, id));
}

static void
dd_enter (void *data, struct wl_data_device *device, uint32_t serial,
          struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y,
          struct wl_data_offer *id)
{
  GDKWDataDevice *dd = data;
  GDKWOffer *offer = offer_from_wl (dd, id);
  GDKWWindowImplWayland *impl = NULL;

  if (surface != NULL && dd->display != NULL)
    impl = _gdk_wayland_impl_from_surface (dd->display, surface);

  dd->rec.last_enter_serial = serial;

  if (offer != NULL)
    {
      offer->kind = GDKW_SEL_DND;
      offer->introduced = TRUE;
      offer->maybe_self = FALSE;
      offer->enter_serial = serial;
    }

  _gdk_wayland_dnd_dest_enter (dd, serial, impl, offer,
                               wl_fixed_to_double (x), wl_fixed_to_double (y));
}

static void
dd_leave (void *data, struct wl_data_device *device)
{
  _gdk_wayland_dnd_dest_leave (data);
}

static void
dd_motion (void *data, struct wl_data_device *device, uint32_t time,
           wl_fixed_t x, wl_fixed_t y)
{
  _gdk_wayland_dnd_dest_motion (data, time,
                                wl_fixed_to_double (x), wl_fixed_to_double (y));
}

static void
dd_drop (void *data, struct wl_data_device *device)
{
  _gdk_wayland_dnd_dest_drop (data);
}

static const struct wl_data_device_listener dd_listener = {
  .data_offer = dd_data_offer,
  .enter = dd_enter,
  .leave = dd_leave,
  .motion = dd_motion,
  .drop = dd_drop,
  .selection = dd_selection
};

static void
pd_data_offer (void *data,
               struct zwp_primary_selection_device_v1 *device,
               struct zwp_primary_selection_offer_v1 *id)
{
  GDKWDataDevice *dd = data;
  GDKWOffer *offer;

  offer = offer_create (dd, GDKW_SEL_PRIMARY, NULL, id);
  if (id != NULL)
    zwp_primary_selection_offer_v1_add_listener (id, &zwp_offer_listener, offer);
}

static void
pd_selection (void *data,
              struct zwp_primary_selection_device_v1 *device,
              struct zwp_primary_selection_offer_v1 *id)
{
  GDKWDataDevice *dd = data;

  dd_set_current_offer (dd, GDKW_SEL_PRIMARY, offer_from_zwp (dd, id));
}

static const struct zwp_primary_selection_device_v1_listener pd_listener = {
  .data_offer = pd_data_offer,
  .selection = pd_selection
};

/* ------------------------------------------------------------------------ */
/* Registry binding, seat readiness                                         */
/* ------------------------------------------------------------------------ */

void
_gdk_wayland_dd_seat_ready (GDKWDisplayWayland *display)
{
  GDKWDataDevice *dd;

  if (display == NULL || display->wl_seat == NULL)
    return;

  dd = _gdk_wayland_display_get_data_device (GDK_DISPLAY_OBJECT (display));
  if (dd->finalized)
    return;

  if (dd->manager != NULL && dd->device == NULL)
    {
      dd->device = wl_data_device_manager_get_data_device (dd->manager,
                                                           display->wl_seat);
      if (dd->device != NULL)
        wl_data_device_add_listener (dd->device, &dd_listener, dd);
    }

  if (dd->primary_manager != NULL && dd->primary_device == NULL)
    {
      dd->primary_device =
        zwp_primary_selection_device_manager_v1_get_device (dd->primary_manager,
                                                            display->wl_seat);
      if (dd->primary_device != NULL)
        zwp_primary_selection_device_v1_add_listener (dd->primary_device,
                                                      &pd_listener, dd);
    }

  dd_flush (dd);
}

void
_gdk_wayland_dd_bind_manager (GDKWDisplayWayland *display,
                              struct wl_registry *registry,
                              uint32_t            name,
                              uint32_t            version)
{
  GDKWDataDevice *dd;

  if (display == NULL)
    return;

  dd = _gdk_wayland_display_get_data_device (GDK_DISPLAY_OBJECT (display));
  if (dd->manager != NULL || dd->finalized)
    return;

  dd->manager = wl_registry_bind (registry, name,
                                  &wl_data_device_manager_interface,
                                  MIN (version, (uint32_t) GDKW_DD_MANAGER_VERSION));
  if (dd->manager == NULL)
    return;

  dd->manager_version = wl_data_device_manager_get_version (dd->manager);
  _gdk_wayland_dd_seat_ready (display);
}

void
_gdk_wayland_dd_bind_primary_manager (GDKWDisplayWayland *display,
                                      struct wl_registry *registry,
                                      uint32_t            name,
                                      uint32_t            version)
{
  GDKWDataDevice *dd;

  if (display == NULL)
    return;

  dd = _gdk_wayland_display_get_data_device (GDK_DISPLAY_OBJECT (display));
  if (dd->primary_manager != NULL || dd->finalized)
    return;

  dd->primary_manager =
    wl_registry_bind (registry, name,
                      &zwp_primary_selection_device_manager_v1_interface, 1);
  if (dd->primary_manager == NULL)
    return;

  _gdk_wayland_dd_seat_ready (display);
}

/* ------------------------------------------------------------------------ */
/* Teardown                                                                 */
/* ------------------------------------------------------------------------ */

/* Releases pipe ops, sources, offers, feeds and (when @proxies) the device and
 * manager proxies.  Callbacks are not called.  Used by display finalize, the
 * test fini and dd_free (the last without proxies). */
static void
dd_teardown (GDKWDataDevice *dd, gboolean proxies, gboolean managers)
{
  while (dd->ops != NULL)
    op_release (dd->ops->data);

  while (dd->sources != NULL)
    source_drop (dd->sources->data, proxies);

  while (dd->offers != NULL)
    {
      GDKWOffer *offer = dd->offers->data;

      /* a reference may live elsewhere (a context): orphan it, never free
       * it under its holder */
      _gdk_wayland_dd_offer_ref (offer);
      offer_kill (offer, proxies, FALSE);
      if (g_list_find (dd->offers, offer) != NULL)
        {
          dd->offers = g_list_remove (dd->offers, offer);
          _gdk_wayland_dd_offer_unref (offer);
        }
      if (offer->ref_count > 1)
        offer->dd = NULL;
      _gdk_wayland_dd_offer_unref (offer);
    }

  dd->selection_offer[GDKW_SEL_CLIPBOARD] = NULL;
  dd->selection_offer[GDKW_SEL_PRIMARY] = NULL;
  dd->dnd.dest_offer = NULL;

  if (dd->feeds != NULL)
    g_hash_table_remove_all (dd->feeds);

  if (!proxies)
    return;

  if (dd->device != NULL)
    {
      if (wl_data_device_get_version (dd->device) >= 2)
        wl_data_device_release (dd->device);
      else
        wl_data_device_destroy (dd->device);
      dd->device = NULL;
    }
  if (dd->primary_device != NULL)
    {
      zwp_primary_selection_device_v1_destroy (dd->primary_device);
      dd->primary_device = NULL;
    }

  if (managers)
    {
      if (dd->manager != NULL)
        {
          wl_data_device_manager_destroy (dd->manager);
          dd->manager = NULL;
        }
      if (dd->primary_manager != NULL)
        {
          zwp_primary_selection_device_manager_v1_destroy (dd->primary_manager);
          dd->primary_manager = NULL;
        }
    }
}

void
_gdk_wayland_dd_display_finalize (GdkDisplay *display)
{
  GDKWDataDevice *dd = dd_peek (display);

  if (dd == NULL || dd->finalized)
    return;

  /* The two pieces release their own sources, offers and pipe ops first. */
  _gdk_wayland_selection_display_finalize (display);
  _gdk_wayland_dnd_display_finalize (display);

  dd_teardown (dd, TRUE, TRUE);
  dd_flush (dd);

  dd->virtual_device = FALSE;
  dd->finalized = TRUE;
}

void
_gdk_wayland_dd_free (gpointer data)
{
  GDKWDataDevice *dd = data;

  if (dd == NULL)
    return;

  if (!dd->finalized)
    {
      /* The display object is already being torn down: do not touch the
       * connection, only release memory and GSources. */
      dd->device = NULL;
      dd->primary_device = NULL;
      dd->manager = NULL;
      dd->primary_manager = NULL;
      dd_teardown (dd, FALSE, FALSE);
    }

  if (dd->feeds != NULL)
    g_hash_table_destroy (dd->feeds);
  g_free (dd);
}

/* ------------------------------------------------------------------------ */
/* Test hooks                                                               */
/* ------------------------------------------------------------------------ */

gboolean
_gdk_wayland_test_dd_init (GdkDisplay *display)
{
  GDKWDataDevice *dd = dd_get (display);

  if (dd == NULL || dd->finalized)
    return FALSE;
  if (dd->virtual_device)
    return TRUE;
  if (dd->device != NULL || dd->primary_device != NULL)
    return FALSE;

  dd->virtual_device = TRUE;
  if (dd->feeds == NULL)
    dd->feeds = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, feed_free);
  if (dd->test_serial == 0)
    dd->test_serial = 0x7000;
  return TRUE;
}

gboolean
_gdk_wayland_test_dd_fini (GdkDisplay *display)
{
  GDKWDataDevice *dd = dd_get_virtual (display);

  if (dd == NULL)
    return FALSE;

  _gdk_wayland_selection_display_finalize (display);
  _gdk_wayland_dnd_display_finalize (display);

  test_offer_arg = NULL;
  dd_teardown (dd, FALSE, FALSE);

  dd->virtual_device = FALSE;
  memset (&dd->rec, 0, sizeof dd->rec);
  dd->test_serial = 0;
  return TRUE;
}

gboolean
_gdk_wayland_test_dd_info (GdkDisplay *display, GDKWTestDDInfo *info)
{
  GDKWDataDevice *dd;
  GDKWSeat *seat;

  if (info == NULL)
    return FALSE;
  if (display == NULL || !GDKW_IS_DISPLAY_WAYLAND (display))
    return FALSE;

  dd = dd_get (display);
  *info = dd->rec;

  info->virtual_device = dd->virtual_device;
  info->have_device = dd->virtual_device || dd->device != NULL;
  info->have_primary = dd->virtual_device || dd->primary_device != NULL;
  info->manager_version = dd->virtual_device ? GDKW_DD_MANAGER_VERSION
                                             : dd->manager_version;
  seat = _gdk_wayland_display_get_seat (display);
  info->seat_serial = seat ? seat->last_serial : 0;
  return TRUE;
}

gboolean
_gdk_wayland_test_dd_reset_requests (GdkDisplay *display)
{
  GDKWDataDevice *dd = dd_get (display);
  GDKWTestDDInfo saved;

  if (dd == NULL)
    return FALSE;

  saved = dd->rec;
  memset (&dd->rec, 0, sizeof dd->rec);
  dd->rec.offers_alive = saved.offers_alive;
  dd->rec.sources_alive = saved.sources_alive;
  dd->rec.pending_reads = saved.pending_reads;
  dd->rec.pending_writes = saved.pending_writes;
  dd->rec.last_enter_serial = saved.last_enter_serial;
  return TRUE;
}

gboolean
_gdk_wayland_test_dd_set_timeouts (GdkDisplay *display,
                                   guint       receive_ms,
                                   guint       send_ms,
                                   guint       probe_ms,
                                   guint       local_ms)
{
  GDKWDataDevice *dd = dd_get (display);

  if (dd == NULL)
    return FALSE;

  if (receive_ms != 0)
    dd->receive_timeout_ms = receive_ms;
  if (send_ms != 0)
    dd->send_timeout_ms = send_ms;
  if (probe_ms != 0)
    dd->probe_timeout_ms = probe_ms;
  if (local_ms != 0)
    dd->local_timeout_ms = local_ms;
  return TRUE;
}

guint32
_gdk_wayland_test_dd_seat_serial (GdkDisplay *display)
{
  GDKWSeat *seat;

  if (display == NULL || !GDKW_IS_DISPLAY_WAYLAND (display))
    return 0;

  seat = _gdk_wayland_display_get_seat (display);
  return seat ? seat->last_serial : 0;
}

guint
_gdk_wayland_test_dd_offer_new (GdkDisplay          *display,
                                GDKWSelKind          kind,
                                guint                version,
                                const gchar * const *mimes)
{
  GDKWDataDevice *dd = dd_get_virtual (display);
  GDKWOffer *offer;
  guint id;
  guint i;

  if (dd == NULL || version < 1 || version > 3 ||
      (kind != GDKW_SEL_CLIPBOARD && kind != GDKW_SEL_PRIMARY &&
       kind != GDKW_SEL_DND))
    return 0;

  id = dd->next_offer_id;

  if (kind == GDKW_SEL_PRIMARY)
    pd_data_offer (dd, NULL, NULL);
  else
    dd_data_offer (dd, NULL, NULL);

  offer = offer_find (dd, id);
  if (offer == NULL)
    return 0;

  offer->kind = kind;
  offer->version = version;

  for (i = 0; mimes != NULL && mimes[i] != NULL; i++)
    {
      if (kind == GDKW_SEL_PRIMARY)
        zwp_offer_offer (offer, NULL, mimes[i]);
      else
        wl_offer_offer (offer, NULL, mimes[i]);
    }

  return id;
}

gboolean
_gdk_wayland_test_dd_offer_data (GdkDisplay   *display,
                                 guint         offer_id,
                                 const gchar  *mime,
                                 const guint8 *data,
                                 gsize         length,
                                 GDKWTestFeed  feed)
{
  GDKWDataDevice *dd = dd_get_virtual (display);

  if (dd == NULL || mime == NULL || offer_find (dd, offer_id) == NULL ||
      (data == NULL && length != 0))
    return FALSE;

  if (dd->feeds == NULL)
    dd->feeds = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, feed_free);

  g_hash_table_replace (dd->feeds, feed_key (offer_id, mime),
                        feed_new (data, length, feed));
  return TRUE;
}

gboolean
_gdk_wayland_test_dd_selection (GdkDisplay *display,
                                GDKWSelKind kind,
                                guint       offer_id)
{
  GDKWDataDevice *dd = dd_get_virtual (display);
  GDKWOffer *offer = NULL;

  if (dd == NULL || (kind != GDKW_SEL_CLIPBOARD && kind != GDKW_SEL_PRIMARY))
    return FALSE;

  if (offer_id != 0)
    {
      offer = offer_find (dd, offer_id);
      if (offer == NULL || offer->dead ||
          (kind == GDKW_SEL_PRIMARY) != (offer->zwp_offer != NULL ||
                                         offer->kind == GDKW_SEL_PRIMARY))
        return FALSE;
    }

  test_offer_arg = offer;
  if (kind == GDKW_SEL_PRIMARY)
    pd_selection (dd, NULL, NULL);
  else
    dd_selection (dd, NULL, NULL);
  test_offer_arg = NULL;
  return TRUE;
}

gboolean
_gdk_wayland_test_dd_offer_source_actions (GdkDisplay *display,
                                           guint       offer_id,
                                           guint32     wl_actions)
{
  GDKWDataDevice *dd = dd_get_virtual (display);
  GDKWOffer *offer = dd ? offer_find (dd, offer_id) : NULL;

  if (offer == NULL || offer->dead || offer->version < 3 ||
      offer->kind == GDKW_SEL_PRIMARY)
    return FALSE;

  wl_offer_source_actions (offer, NULL, wl_actions);
  return TRUE;
}

gboolean
_gdk_wayland_test_dd_offer_action (GdkDisplay *display,
                                   guint       offer_id,
                                   guint32     wl_action)
{
  GDKWDataDevice *dd = dd_get_virtual (display);
  GDKWOffer *offer = dd ? offer_find (dd, offer_id) : NULL;

  if (offer == NULL || offer->dead || offer->version < 3 ||
      offer->kind == GDKW_SEL_PRIMARY)
    return FALSE;

  wl_offer_action (offer, NULL, wl_action);
  return TRUE;
}

gboolean
_gdk_wayland_test_dd_dnd_enter (GdkDisplay *display,
                                GdkWindow  *window,
                                guint       offer_id,
                                gdouble     x,
                                gdouble     y,
                                guint32    *serial)
{
  GDKWDataDevice *dd = dd_get_virtual (display);
  GDKWWindowImplWayland *impl;
  GDKWOffer *offer = NULL;
  guint32 enter_serial;

  if (dd == NULL || window == NULL)
    return FALSE;

  impl = _gdk_wayland_window_get_impl (window);
  if (impl == NULL || impl->wl_surface == NULL)
    return FALSE;

  if (offer_id != 0)
    {
      offer = offer_find (dd, offer_id);
      if (offer == NULL || offer->dead || offer->kind != GDKW_SEL_DND)
        return FALSE;
    }

  enter_serial = ++dd->test_serial;
  if (serial != NULL)
    *serial = enter_serial;

  test_offer_arg = offer;
  dd_enter (dd, NULL, enter_serial, impl->wl_surface,
            wl_fixed_from_double (x), wl_fixed_from_double (y), NULL);
  test_offer_arg = NULL;
  return TRUE;
}

gboolean
_gdk_wayland_test_dd_dnd_motion (GdkDisplay *display,
                                 guint32     time,
                                 gdouble     x,
                                 gdouble     y)
{
  GDKWDataDevice *dd = dd_get_virtual (display);

  if (dd == NULL)
    return FALSE;

  dd_motion (dd, NULL, time, wl_fixed_from_double (x), wl_fixed_from_double (y));
  return TRUE;
}

gboolean
_gdk_wayland_test_dd_dnd_leave (GdkDisplay *display)
{
  GDKWDataDevice *dd = dd_get_virtual (display);

  if (dd == NULL)
    return FALSE;

  dd_leave (dd, NULL);
  return TRUE;
}

gboolean
_gdk_wayland_test_dd_dnd_drop (GdkDisplay *display)
{
  GDKWDataDevice *dd = dd_get_virtual (display);

  if (dd == NULL)
    return FALSE;

  dd_drop (dd, NULL);
  return TRUE;
}

gboolean
_gdk_wayland_test_dd_source_send (GdkDisplay  *display,
                                  GDKWSelKind  kind,
                                  const gchar *mime,
                                  guint8     **data,
                                  gsize       *length)
{
  GDKWDataDevice *dd = dd_get_virtual (display);
  GDKWSource *src;
  GByteArray *bytes;
  gint64 deadline;
  int fds[2];
  gboolean eof = FALSE;

  if (data != NULL)
    *data = NULL;
  if (length != NULL)
    *length = 0;

  if (dd == NULL || mime == NULL || data == NULL || length == NULL ||
      kind < 0 || kind >= GDKW_SEL_N_KINDS)
    return FALSE;

  src = dd->source[kind];
  if (src == NULL || src->freed || src->cancelled)
    return FALSE;

  if (!g_unix_open_pipe (fds, O_CLOEXEC, NULL))
    return FALSE;
  if (!g_unix_set_fd_nonblocking (fds[0], TRUE, NULL))
    {
      close (fds[0]);
      close (fds[1]);
      return FALSE;
    }

  bytes = g_byte_array_new ();

  /* the REAL listener; it takes the write end.  The source may be gone
   * afterwards (a handler can free it), so do not touch @src again. */
  if (kind == GDKW_SEL_PRIMARY)
    zwp_source_send (src, NULL, mime, fds[1]);
  else
    wl_source_send (src, NULL, mime, fds[1]);

  deadline = g_get_monotonic_time () + 3 * G_USEC_PER_SEC;

  while (!eof && g_get_monotonic_time () < deadline)
    {
      guchar buf[GDKW_PIPE_CHUNK];
      struct pollfd pfd;
      gssize n;
      gboolean progress;

      progress = g_main_context_iteration (NULL, FALSE);

      do
        n = read (fds[0], buf, sizeof buf);
      while (n < 0 && errno == EINTR);

      if (n > 0)
        {
          g_byte_array_append (bytes, buf, (guint) n);
          continue;
        }
      if (n == 0)
        {
          eof = TRUE;
          break;
        }
      if (errno != EAGAIN && errno != EWOULDBLOCK)
        break;

      if (!progress)
        {
          pfd.fd = fds[0];
          pfd.events = POLLIN;
          pfd.revents = 0;
          poll (&pfd, 1, 5);
        }
    }

  close (fds[0]);

  if (!eof)
    {
      g_byte_array_free (bytes, TRUE);
      return FALSE;
    }

  *length = bytes->len;
  g_byte_array_append (bytes, (const guint8 *) "", 1);
  *data = g_byte_array_free (bytes, FALSE);
  return TRUE;
}

gboolean
_gdk_wayland_test_dd_source_event (GdkDisplay         *display,
                                   GDKWSelKind         kind,
                                   GDKWTestSourceEvent event,
                                   const gchar        *mime,
                                   guint32             wl_action)
{
  GDKWDataDevice *dd = dd_get_virtual (display);
  GDKWSource *src;

  if (dd == NULL || kind < 0 || kind >= GDKW_SEL_N_KINDS)
    return FALSE;

  src = dd->source[kind];
  if (src == NULL || src->freed || src->cancelled)
    return FALSE;

  if (kind != GDKW_SEL_DND && event != GDKW_TEST_SRC_CANCELLED)
    return FALSE;

  switch (event)
    {
    case GDKW_TEST_SRC_TARGET:
      wl_source_target (src, NULL, mime);
      return TRUE;
    case GDKW_TEST_SRC_ACTION:
      wl_source_action (src, NULL, wl_action);
      return TRUE;
    case GDKW_TEST_SRC_DROP_PERFORMED:
      wl_source_dnd_drop_performed (src, NULL);
      return TRUE;
    case GDKW_TEST_SRC_FINISHED:
      wl_source_dnd_finished (src, NULL);
      return TRUE;
    case GDKW_TEST_SRC_CANCELLED:
      source_do_cancelled (src);
      return TRUE;
    default:
      break;
    }

  return FALSE;
}
