#ifndef MINTTY_PERF_H
#define MINTTY_PERF_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/*
 * Lightweight paint/update instrumentation.
 *
 * The counters are intentionally aggregated per display update.  This keeps
 * the log useful under heavy output without changing rendering behaviour more
 * than the measured timing/counter overhead itself.
 */
struct mintty_perf_frame {
  uint64_t update_no;
  char     source[16];

  int cols;
  int rows;
  int allrows;
  int cell_width;
  int cell_height;
  int display_speedup;
  int display_buffering;
  int ligatures;
  int font_render;
  int output_lines_scrolled;
  int output_speed;
  int update_skipped;
  int disptop;

  int paint_dirty_top;
  int paint_dirty_bot;

  uint64_t update_ticks;
  uint64_t getdc_ticks;
  uint64_t release_dc_ticks;
  uint64_t beginpaint_ticks;
  uint64_t endpaint_ticks;
  uint64_t search_ticks;
  uint64_t paint_buffer_begin_ticks;
  uint64_t term_paint_ticks;
  uint64_t paint_buffer_end_ticks;
  uint64_t winimgs_paint_ticks;
  uint64_t padding_paint_ticks;
  uint64_t scrollbar_ticks;
  uint64_t caret_ticks;

  uint64_t buffer_begin_calls;
  uint64_t buffer_used;
  uint64_t buffer_recreate;
  uint64_t buffer_stale_full_repaint;
  uint64_t buffer_reject_disabled;
  uint64_t buffer_reject_tek;
  uint64_t buffer_reject_horclip;
  uint64_t buffer_reject_imgs;
  uint64_t buffer_reject_size;
  uint64_t buffer_reject_create;

  uint64_t bitblt_calls;
  uint64_t bitblt_pixels;
  uint64_t bitblt_width;
  uint64_t bitblt_height;
  uint64_t bitblt_ticks;

  uint64_t term_paint_calls;
  uint64_t paint_lines_seen;
  uint64_t paint_cells_seen;
  uint64_t paint_dirty_lines;
  uint64_t paint_dirty_cells;
  uint64_t paint_overlay_lines;
  uint64_t paint_out_text_calls;
  uint64_t paint_out_text_chars;
  uint64_t paint_out_text_ascii_chars;
  uint64_t paint_out_text_nonascii_chars;
  uint64_t paint_out_text_emoji_chars;
  uint64_t run_breaks;
  uint64_t run_breaks_attr;
  uint64_t run_breaks_narrow;
  uint64_t run_breaks_emoji;
  uint64_t run_breaks_combining;
  uint64_t run_breaks_surrogate;
  uint64_t run_breaks_clean_cell;
  uint64_t run_breaks_ascii_boundary;
  uint64_t run_breaks_bidi;
  uint64_t run_breaks_bloom;
  uint64_t bidi_class_calls;
  uint64_t bidi_ascii_cells;
  uint64_t bidi_nonascii_cells;
  uint64_t bidi_ascii_pair_no_breaks;

  uint64_t win_text_calls;
  uint64_t win_text_chars;
  uint64_t win_text_ascii_chars;
  uint64_t win_text_nonascii_chars;
  uint64_t win_text_phase0_calls;
  uint64_t win_text_phase1_calls;
  uint64_t win_text_phase2_calls;
  uint64_t win_text_clearpad_calls;
  uint64_t win_text_boxpower_calls;
  uint64_t win_text_boxpower_chars;
  uint64_t win_text_boxcoded_calls;
  uint64_t win_text_boxcoded_chars;
  uint64_t win_text_vt52fraction_calls;
  uint64_t win_text_vt52fraction_chars;
  uint64_t win_text_dectcs_calls;
  uint64_t win_text_dectcs_chars;
  uint64_t win_text_combining_calls;
  uint64_t win_text_combining_double_calls;
  uint64_t win_text_rtl_calls;
  uint64_t win_text_sea_calls;
  uint64_t win_text_skip_invisible_calls;
  uint64_t win_text_skip_origtext_calls;
  uint64_t win_text_textout_plain_calls;
  uint64_t win_text_textout_combining_calls;
  uint64_t win_text_overstrike_iterations;
  uint64_t win_text_ticks;
  uint64_t win_text_font_resolve_ticks;
  uint64_t win_text_attr_colour_ticks;
  uint64_t win_text_origtext_alloc_calls;
  uint64_t win_text_origtext_alloc_chars;
  uint64_t win_text_origtext_alloc_ticks;
  uint64_t win_text_origtext_free_ticks;
  uint64_t win_text_dxs_ticks;
  uint64_t win_text_ulen_ticks;
  uint64_t win_text_uniscribe_decision_ticks;
  uint64_t win_text_background_ticks;
  uint64_t win_text_coord_line_ticks;
  uint64_t win_text_coord_char_ticks;
  uint64_t win_text_bkmode_ticks;
  uint64_t win_text_textout_path_ticks;
  uint64_t win_text_textout_end_ticks;
  uint64_t win_text_coord_restore_ticks;
  uint64_t win_text_selfdraw_ticks;
  uint64_t win_text_selfdraw_vt52_ticks;
  uint64_t win_text_selfdraw_boxpower_ticks;
  uint64_t win_text_selfdraw_boxcoded_ticks;
  uint64_t win_text_selfdraw_resource_ticks;
  uint64_t win_text_selfdraw_teardown_ticks;
  uint64_t win_text_clip_set_calls;
  uint64_t win_text_clip_clear_calls;
  uint64_t win_text_clip_ticks;
  uint64_t win_text_selfdraw_fillrect_calls;
  uint64_t win_text_selfdraw_fillrect_pixels;
  uint64_t win_text_selfdraw_fillrect_ticks;
  uint64_t win_text_selfdraw_linedraw_calls;
  uint64_t win_text_selfdraw_line_ops;
  uint64_t win_text_selfdraw_rect_ops;
  uint64_t win_text_selfdraw_polygon_ops;
  uint64_t win_text_selfdraw_chord_ops;
  uint64_t win_text_selfdraw_anglearc_ops;
  uint64_t win_text_selfdraw_setpixel_ops;
  uint64_t win_text_gdi_create_pen_calls;
  uint64_t win_text_gdi_create_brush_calls;
  uint64_t win_text_gdi_create_rgn_calls;
  uint64_t win_text_gdi_delete_object_calls;
  uint64_t win_text_gdi_select_object_calls;
  uint64_t win_text_gdi_select_clip_calls;
  uint64_t set_bk_mode_calls;
  uint64_t set_bk_mode_ticks;
  uint64_t set_dc_brush_color_calls;
  uint64_t set_dc_brush_color_ticks;
  uint64_t fill_background_calls;
  uint64_t fill_background_ticks;
  uint64_t select_font_calls;
  uint64_t select_font_ticks;
  uint64_t set_text_color_calls;
  uint64_t set_text_color_ticks;
  uint64_t set_bk_color_calls;
  uint64_t set_bk_color_ticks;

  uint64_t fillrect_calls;
  uint64_t fillrect_pixels;
  uint64_t fillrect_ticks;

  uint64_t text_out_start_calls;
  uint64_t text_out_start_chars;
  uint64_t text_out_start_ticks;
  uint64_t uniscribe_analyse_calls;
  uint64_t uniscribe_analyse_chars;
  uint64_t uniscribe_analyse_failures;
  uint64_t uniscribe_analyse_ticks;
  uint64_t script_string_out_calls;
  uint64_t script_string_out_chars;
  uint64_t script_string_out_ticks;
  uint64_t ext_text_out_calls;
  uint64_t ext_text_out_chars;
  uint64_t ext_text_out_ticks;
  uint64_t script_string_free_ticks;

  uint64_t wcw_calls;
  uint64_t wcw_ascii_fast;
  uint64_t wcw_out_of_range;
  uint64_t wcw_cache_hits;
  uint64_t wcw_cache_misses;
  uint64_t wcw_uncached_ticks;

  uint64_t scroll_calls;
  uint64_t scroll_lines;
  uint64_t scroll_forward_calls;
  uint64_t scroll_backward_calls;
  uint64_t scroll_lrmargin_rejects;
  uint64_t scroll_full_width_calls;
  uint64_t scroll_visible_calls;
  uint64_t scroll_visible_lines;
  uint64_t scroll_scrollback_calls;

  uint64_t invalidate_calls;
  uint64_t invalidate_cells;
  uint64_t invalidate_full_calls;
};

struct mintty_perf_state {
  bool enabled;
  bool in_frame;
  bool initialised;
  FILE *log;
  uint64_t frequency;
  uint64_t next_update_no;
  uint64_t flush_countdown;
  uint64_t frame_start_ticks;
  struct mintty_perf_frame frame;
  struct mintty_perf_frame pending;
};

extern struct mintty_perf_state mintty_perf;

void mintty_perf_ensure(void);
long long mintty_perf_ticks(void);
void mintty_perf_update_begin(const char *source);
void mintty_perf_update_end(void);
void mintty_perf_note_update_skip(int skipped, int speedup, int output_speed,
                                  int lines_scrolled, bool iconic,
                                  int suspend_update);

#define PERF_COUNT(field, value) \
  do { \
    if (mintty_perf.enabled) { \
      (mintty_perf.in_frame ? &mintty_perf.frame : &mintty_perf.pending)->field += (uint64_t)(value); \
    } \
  } while (0)
#define PERF_SET(field, value) \
  do { if (mintty_perf.enabled && mintty_perf.in_frame) { mintty_perf.frame.field = (value); } } while (0)
#define PERF_ADD_TICKS(field, value) \
  do { \
    if (mintty_perf.enabled) { \
      (mintty_perf.in_frame ? &mintty_perf.frame : &mintty_perf.pending)->field += (uint64_t)(value); \
    } \
  } while (0)

#endif
