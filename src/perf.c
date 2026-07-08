// perf.c (part of mintty)
// Paint/update instrumentation for local performance investigations.
// Licensed under the terms of the GNU General Public License v3 or later.

#include "perf.h"

#include <winbase.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

struct mintty_perf_state mintty_perf;

static bool
mintty_perf_env_enabled(void)
{
  const char *env = getenv("MINTTY_PERF");
  if (!env || !*env) {
    return true;
  }
  return strcmp(env, "0") != 0 && strcasecmp(env, "off") != 0 &&
         strcasecmp(env, "false") != 0 && strcasecmp(env, "no") != 0;
}

static void
mintty_perf_close(void)
{
  if (mintty_perf.log) {
    fflush(mintty_perf.log);
    fclose(mintty_perf.log);
    mintty_perf.log = null;
  }
}

static void
mintty_perf_init(void)
{
  if (mintty_perf.initialised) {
    return;
  }
  mintty_perf.initialised = true;
  mintty_perf.enabled = mintty_perf_env_enabled();
  if (!mintty_perf.enabled) {
    return;
  }

  LARGE_INTEGER frequency;
  if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
    mintty_perf.enabled = false;
    return;
  }
  mintty_perf.frequency = (uint64_t)frequency.QuadPart;
  mintty_perf.next_update_no = 1;
  mintty_perf.flush_countdown = 60;

  CreateDirectoryA("C:\\Temp", null);

  char path[128];
  snprintf(path, sizeof path, "C:\\Temp\\mintty.perf.%lu.log",
           (unsigned long)GetCurrentProcessId());
  mintty_perf.log = fopen(path, "a");
  if (!mintty_perf.log) {
    mintty_perf.enabled = false;
    return;
  }
  setvbuf(mintty_perf.log, null, _IOFBF, 128 * 1024);
  atexit(mintty_perf_close);

  fprintf(mintty_perf.log,
          "# mintty perf log v1 pid=%lu frequency=%llu path=%s\n",
          (unsigned long)GetCurrentProcessId(),
          (unsigned long long)mintty_perf.frequency, path);
}

void
mintty_perf_ensure(void)
{
  mintty_perf_init();
}

long long
mintty_perf_ticks(void)
{
  if (!mintty_perf.enabled) {
    return 0;
  }
  LARGE_INTEGER counter;
  if (!QueryPerformanceCounter(&counter)) {
    return 0;
  }
  return (long long)counter.QuadPart;
}

static unsigned long long
mintty_perf_us(uint64_t ticks)
{
  if (!mintty_perf.frequency) {
    return 0;
  }
  return (unsigned long long)((ticks * 1000000ULL) / mintty_perf.frequency);
}

static void
mintty_perf_put_u64(const char *name, uint64_t value)
{
  fprintf(mintty_perf.log, " %s=%llu", name, (unsigned long long)value);
}

static void
mintty_perf_put_i32(const char *name, int value)
{
  fprintf(mintty_perf.log, " %s=%d", name, value);
}

static void
mintty_perf_emit_frame(void)
{
  const struct mintty_perf_frame *f = &mintty_perf.frame;

#define PUT_U64(field) mintty_perf_put_u64(#field, f->field)
#define PUT_I32(field) mintty_perf_put_i32(#field, f->field)
#define PUT_US(field) mintty_perf_put_u64(#field "_us", mintty_perf_us(f->field))

  fprintf(mintty_perf.log, "update=%llu source=%s",
          (unsigned long long)f->update_no, f->source);
  PUT_I32(cols);
  PUT_I32(rows);
  PUT_I32(allrows);
  PUT_I32(cell_width);
  PUT_I32(cell_height);
  PUT_I32(display_speedup);
  PUT_I32(display_buffering);
  PUT_I32(ligatures);
  PUT_I32(font_render);
  PUT_I32(output_lines_scrolled);
  PUT_I32(output_speed);
  PUT_I32(update_skipped);
  PUT_I32(disptop);
  PUT_I32(paint_dirty_top);
  PUT_I32(paint_dirty_bot);

  PUT_US(update_ticks);
  PUT_US(getdc_ticks);
  PUT_US(release_dc_ticks);
  PUT_US(beginpaint_ticks);
  PUT_US(endpaint_ticks);
  PUT_US(search_ticks);
  PUT_US(paint_buffer_begin_ticks);
  PUT_US(term_paint_ticks);
  PUT_US(paint_buffer_end_ticks);
  PUT_US(winimgs_paint_ticks);
  PUT_US(padding_paint_ticks);
  PUT_US(scrollbar_ticks);
  PUT_US(caret_ticks);

  PUT_U64(buffer_begin_calls);
  PUT_U64(buffer_used);
  PUT_U64(buffer_recreate);
  PUT_U64(buffer_stale_full_repaint);
  PUT_U64(buffer_reject_disabled);
  PUT_U64(buffer_reject_tek);
  PUT_U64(buffer_reject_horclip);
  PUT_U64(buffer_reject_imgs);
  PUT_U64(buffer_reject_size);
  PUT_U64(buffer_reject_create);

  PUT_U64(bitblt_calls);
  PUT_U64(bitblt_pixels);
  PUT_U64(bitblt_width);
  PUT_U64(bitblt_height);
  PUT_US(bitblt_ticks);

  PUT_U64(term_paint_calls);
  PUT_U64(paint_lines_seen);
  PUT_U64(paint_cells_seen);
  PUT_U64(paint_dirty_lines);
  PUT_U64(paint_dirty_cells);
  PUT_U64(paint_overlay_lines);
  PUT_U64(paint_out_text_calls);
  PUT_U64(paint_out_text_chars);
  PUT_U64(paint_out_text_ascii_chars);
  PUT_U64(paint_out_text_nonascii_chars);
  PUT_U64(paint_out_text_emoji_chars);
  PUT_U64(run_breaks);
  PUT_U64(run_breaks_attr);
  PUT_U64(run_breaks_narrow);
  PUT_U64(run_breaks_emoji);
  PUT_U64(run_breaks_combining);
  PUT_U64(run_breaks_surrogate);
  PUT_U64(run_breaks_clean_cell);
  PUT_U64(run_breaks_ascii_boundary);
  PUT_U64(run_breaks_bidi);
  PUT_U64(bidi_class_calls);
  PUT_U64(bidi_ascii_cells);
  PUT_U64(bidi_nonascii_cells);
  PUT_U64(bidi_ascii_pair_no_breaks);

  PUT_U64(win_text_calls);
  PUT_U64(win_text_chars);
  PUT_U64(win_text_ascii_chars);
  PUT_U64(win_text_nonascii_chars);
  PUT_U64(win_text_phase0_calls);
  PUT_U64(win_text_phase1_calls);
  PUT_U64(win_text_phase2_calls);
  PUT_U64(win_text_clearpad_calls);
  PUT_U64(win_text_boxpower_calls);
  PUT_U64(win_text_boxpower_chars);
  PUT_U64(win_text_boxcoded_calls);
  PUT_U64(win_text_boxcoded_chars);
  PUT_U64(win_text_vt52fraction_calls);
  PUT_U64(win_text_vt52fraction_chars);
  PUT_U64(win_text_dectcs_calls);
  PUT_U64(win_text_dectcs_chars);
  PUT_U64(win_text_combining_calls);
  PUT_U64(win_text_combining_double_calls);
  PUT_U64(win_text_rtl_calls);
  PUT_U64(win_text_sea_calls);
  PUT_U64(win_text_skip_invisible_calls);
  PUT_U64(win_text_skip_origtext_calls);
  PUT_U64(win_text_textout_plain_calls);
  PUT_U64(win_text_textout_combining_calls);
  PUT_U64(win_text_overstrike_iterations);
  PUT_US(win_text_ticks);
  PUT_US(win_text_font_resolve_ticks);
  PUT_US(win_text_attr_colour_ticks);
  PUT_U64(win_text_origtext_alloc_calls);
  PUT_U64(win_text_origtext_alloc_chars);
  PUT_US(win_text_origtext_alloc_ticks);
  PUT_US(win_text_origtext_free_ticks);
  PUT_US(win_text_dxs_ticks);
  PUT_US(win_text_ulen_ticks);
  PUT_US(win_text_uniscribe_decision_ticks);
  PUT_US(win_text_background_ticks);
  PUT_US(win_text_coord_line_ticks);
  PUT_US(win_text_coord_char_ticks);
  PUT_US(win_text_bkmode_ticks);
  PUT_US(win_text_textout_path_ticks);
  PUT_US(win_text_textout_end_ticks);
  PUT_US(win_text_coord_restore_ticks);
  PUT_US(win_text_selfdraw_ticks);
  PUT_US(win_text_selfdraw_vt52_ticks);
  PUT_US(win_text_selfdraw_boxpower_ticks);
  PUT_US(win_text_selfdraw_boxcoded_ticks);
  PUT_US(win_text_selfdraw_resource_ticks);
  PUT_US(win_text_selfdraw_teardown_ticks);
  PUT_U64(win_text_clip_set_calls);
  PUT_U64(win_text_clip_clear_calls);
  PUT_US(win_text_clip_ticks);
  PUT_U64(win_text_selfdraw_fillrect_calls);
  PUT_U64(win_text_selfdraw_fillrect_pixels);
  PUT_US(win_text_selfdraw_fillrect_ticks);
  PUT_U64(win_text_selfdraw_linedraw_calls);
  PUT_U64(win_text_selfdraw_line_ops);
  PUT_U64(win_text_selfdraw_rect_ops);
  PUT_U64(win_text_selfdraw_polygon_ops);
  PUT_U64(win_text_selfdraw_chord_ops);
  PUT_U64(win_text_selfdraw_anglearc_ops);
  PUT_U64(win_text_selfdraw_setpixel_ops);
  PUT_U64(win_text_gdi_create_pen_calls);
  PUT_U64(win_text_gdi_create_brush_calls);
  PUT_U64(win_text_gdi_create_rgn_calls);
  PUT_U64(win_text_gdi_delete_object_calls);
  PUT_U64(win_text_gdi_select_object_calls);
  PUT_U64(win_text_gdi_select_clip_calls);
  PUT_U64(set_bk_mode_calls);
  PUT_US(set_bk_mode_ticks);
  PUT_U64(set_dc_brush_color_calls);
  PUT_US(set_dc_brush_color_ticks);
  PUT_U64(fill_background_calls);
  PUT_US(fill_background_ticks);
  PUT_U64(select_font_calls);
  PUT_US(select_font_ticks);
  PUT_U64(set_text_color_calls);
  PUT_US(set_text_color_ticks);
  PUT_U64(set_bk_color_calls);
  PUT_US(set_bk_color_ticks);

  PUT_U64(fillrect_calls);
  PUT_U64(fillrect_pixels);
  PUT_US(fillrect_ticks);

  PUT_U64(text_out_start_calls);
  PUT_U64(text_out_start_chars);
  PUT_US(text_out_start_ticks);
  PUT_U64(uniscribe_analyse_calls);
  PUT_U64(uniscribe_analyse_chars);
  PUT_U64(uniscribe_analyse_failures);
  PUT_US(uniscribe_analyse_ticks);
  PUT_U64(script_string_out_calls);
  PUT_U64(script_string_out_chars);
  PUT_US(script_string_out_ticks);
  PUT_U64(ext_text_out_calls);
  PUT_U64(ext_text_out_chars);
  PUT_US(ext_text_out_ticks);
  PUT_US(script_string_free_ticks);

  PUT_U64(wcw_calls);
  PUT_U64(wcw_ascii_fast);
  PUT_U64(wcw_out_of_range);
  PUT_U64(wcw_cache_hits);
  PUT_U64(wcw_cache_misses);
  PUT_US(wcw_uncached_ticks);

  PUT_U64(scroll_calls);
  PUT_U64(scroll_lines);
  PUT_U64(scroll_forward_calls);
  PUT_U64(scroll_backward_calls);
  PUT_U64(scroll_lrmargin_rejects);
  PUT_U64(scroll_full_width_calls);
  PUT_U64(scroll_visible_calls);
  PUT_U64(scroll_visible_lines);
  PUT_U64(scroll_scrollback_calls);

  PUT_U64(invalidate_calls);
  PUT_U64(invalidate_cells);
  PUT_U64(invalidate_full_calls);
  fputc('\n', mintty_perf.log);

#undef PUT_US
#undef PUT_I32
#undef PUT_U64

  if (mintty_perf.flush_countdown) {
    mintty_perf.flush_countdown--;
  }
  if (!mintty_perf.flush_countdown) {
    fflush(mintty_perf.log);
    mintty_perf.flush_countdown = 60;
  }
}

void
mintty_perf_update_begin(const char *source)
{
  mintty_perf_init();
  if (!mintty_perf.enabled) {
    return;
  }
  if (mintty_perf.in_frame) {
    mintty_perf_update_end();
  }
  struct mintty_perf_frame pending = mintty_perf.pending;
  memset(&mintty_perf.pending, 0, sizeof mintty_perf.pending);
  memset(&mintty_perf.frame, 0, sizeof mintty_perf.frame);
  mintty_perf.frame = pending;
  mintty_perf.frame.update_no = mintty_perf.next_update_no++;
  snprintf(mintty_perf.frame.source, sizeof mintty_perf.frame.source, "%s",
           source ? source : "?");
  mintty_perf.frame_start_ticks = (uint64_t)mintty_perf_ticks();
  mintty_perf.in_frame = true;
}

void
mintty_perf_update_end(void)
{
  if (!mintty_perf.enabled || !mintty_perf.in_frame) {
    return;
  }
  uint64_t end_ticks = (uint64_t)mintty_perf_ticks();
  mintty_perf.frame.update_ticks += end_ticks - mintty_perf.frame_start_ticks;
  mintty_perf.in_frame = false;
  mintty_perf_emit_frame();
}

void
mintty_perf_note_update_skip(int skipped, int speedup, int output_speed,
                             int lines_scrolled, bool iconic,
                             int suspend_update)
{
  mintty_perf_init();
  if (!mintty_perf.enabled) {
    return;
  }
  fprintf(mintty_perf.log,
          "skip=%llu skipped=%d display_speedup=%d output_speed=%d "
          "lines_scrolled=%d iconic=%d suspend_update=%d\n",
          (unsigned long long)mintty_perf.next_update_no++, skipped, speedup,
          output_speed, lines_scrolled, iconic ? 1 : 0, suspend_update);
  if (mintty_perf.flush_countdown) {
    mintty_perf.flush_countdown--;
  }
  if (!mintty_perf.flush_countdown) {
    fflush(mintty_perf.log);
    mintty_perf.flush_countdown = 60;
  }
}
