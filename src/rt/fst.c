//
//  Copyright (C) 2013  Nick Gasson
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "util.h"
#include "rt.h"
#include "tree.h"
#include "common.h"
#include "fstapi.h"

#include <assert.h>

static tree_t   fst_top;
static void    *fst_ctx;
static ident_t  fst_data_i;
static ident_t  std_logic_i;
static ident_t  std_ulogic_i;
static ident_t  std_bool_i;
static ident_t  std_char_i;
static ident_t  std_bit_i;
static ident_t  natural_i;
static ident_t  positive_i;
static ident_t  fst_dir_i;
static ident_t  unsigned_i;
static ident_t  signed_i;
static uint64_t last_time;

typedef struct fst_data fst_data_t;

typedef void (*fst_fmt_fn_t)(tree_t, watch_t *, fst_data_t *);

struct fst_data {
   fstHandle     handle;
   fst_fmt_fn_t  fmt;
   range_kind_t  dir;
   const char   *map;
   size_t        size;
   watch_t      *watch;
};

static void fst_close(void)
{
   fstWriterEmitTimeChange(fst_ctx, rt_now());
   fstWriterClose(fst_ctx);
}

static void fst_fmt_int(tree_t decl, watch_t *w, fst_data_t *data)
{
   uint64_t val;
   rt_signal_value(w, &val, 1, false);

   char buf[data->size + 1];
   for (size_t i = 0; i < data->size; i++)
      buf[data->size - 1 - i] = (val & (1 << i)) ? '1' : '0';
   buf[data->size] = '\0';

   fstWriterEmitValueChange(fst_ctx, data->handle, buf);
}

static void fst_fmt_chars(tree_t decl, watch_t *w, fst_data_t *data)
{
   const int nvals = data->size;
   uint64_t vals[nvals];
   rt_signal_value(w, vals, data->size, false);

   char buf[nvals + 1];
   if (likely(data->map != NULL)) {
      for (int i = 0; i < nvals; i++)
         buf[i] = data->map[vals[(data->dir == RANGE_TO) ? i : nvals - i - 1]];
      buf[nvals] = '\0';
      fstWriterEmitValueChange(fst_ctx, data->handle, buf);
   }
   else {
      for (int i = 0; i < nvals; i++)
         buf[i] = vals[(data->dir == RANGE_TO) ? i : nvals - i - 1];
      buf[nvals] = '\0';
      fstWriterEmitVariableLengthValueChange(
         fst_ctx, data->handle, buf, data->size);
   }
}

static void fst_fmt_enum(tree_t decl, watch_t *w, fst_data_t *data)
{
   uint64_t val;
   rt_signal_value(w, &val, 1, false);

   tree_t lit = type_enum_literal(tree_type(decl), val);
   const char *str = istr(tree_ident(lit));

   fstWriterEmitVariableLengthValueChange(
      fst_ctx, data->handle, str, strlen(str));
}

static void fst_event_cb(uint64_t now, tree_t decl, watch_t *w)
{
   if (now != last_time) {
      fstWriterEmitTimeChange(fst_ctx, now);
      last_time = now;
   }

   fst_data_t *data = tree_attr_ptr(decl, fst_data_i);
   if (likely(data != NULL))
      (*data->fmt)(decl, w, data);
}

static bool fst_can_fmt_chars(type_t type, fst_data_t *data,
                              enum fstVarType *vt,
                              enum fstSupplimentalDataType *sdt)
{
   type_t base = type_base_recur(type);
   ident_t name = type_ident(base);
   if (name == std_ulogic_i) {
      if (type_ident(type) == std_logic_i)
         *sdt = (data->size > 1) ?
            FST_SDT_VHDL_STD_LOGIC_VECTOR : FST_SDT_VHDL_STD_LOGIC;
      else
         *sdt = (data->size > 1) ?
            FST_SDT_VHDL_STD_ULOGIC_VECTOR : FST_SDT_VHDL_STD_ULOGIC;
      *vt = FST_VT_SV_LOGIC;
      data->fmt = fst_fmt_chars;
      data->map = "UX01ZWLH-";
      return true;
   }
   else if (name == std_bit_i) {
      *sdt = FST_SDT_VHDL_BIT;
      *vt  = FST_VT_SV_LOGIC;
      data->fmt = fst_fmt_chars;
      data->map = "01";
      return true;
   }
   else if ((name == std_char_i) && (data->size > 0)) {
      *sdt = FST_SDT_VHDL_STRING;
      *vt  = FST_VT_GEN_STRING;
      data->fmt = fst_fmt_chars;
      data->map = NULL;
      return true;
   }
   else
      return false;
}

static void fst_process_signal(tree_t d)
{
   type_t type = tree_type(d);
   type_t base = type_base_recur(type);

   fst_data_t *data = xmalloc(sizeof(fst_data_t));
   memset(data, '\0', sizeof(fst_data_t));

   enum fstVarType vt;
   enum fstSupplimentalDataType sdt;
   if (type_is_array(type)) {
      range_t r = type_dim(type, 0);

      int64_t low, high;
      range_bounds(r, &low, &high);

      data->dir  = r.kind;
      data->size = high - low + 1;

      type_t elem = type_elem(type);
      if (!fst_can_fmt_chars(elem, data, &vt, &sdt)) {
         warn_at(tree_loc(d), "cannot represent arrays of type %s "
                 "in FST format", type_pp(elem));
         free(data);
         return;
      }
      else {
         ident_t ident = type_ident(base);
         if (ident == unsigned_i)
            sdt = FST_SDT_VHDL_UNSIGNED;
         else if (ident == signed_i)
            sdt = FST_SDT_VHDL_SIGNED;
      }
   }
   else {
      switch (type_kind(base)) {
      case T_INTEGER:
         {
            ident_t ident = type_ident(type);
            if (ident == natural_i)
               sdt = FST_SDT_VHDL_NATURAL;
            else if (ident == positive_i)
               sdt = FST_SDT_VHDL_POSITIVE;
            else
               sdt = FST_SDT_VHDL_INTEGER;

            vt = FST_VT_VCD_INTEGER;
            data->size = 32;
            data->fmt  = fst_fmt_int;
         }
         break;

      case T_ENUM:
         if (!fst_can_fmt_chars(type, data, &vt, &sdt)) {
            ident_t ident = type_ident(base);
            if (ident == std_bool_i)
               sdt = FST_SDT_VHDL_BOOLEAN;
            else if (ident == std_char_i)
               sdt = FST_SDT_VHDL_CHARACTER;
            else
               sdt = FST_SDT_NONE;

            vt = FST_VT_GEN_STRING;
            data->size = 0;
            data->fmt  = fst_fmt_enum;
         }
         else
            data->size = 1;
         break;

      default:
         warn_at(tree_loc(d), "cannot represent type %s in FST format",
                 type_pp(type));
         free(data);
         return;
      }
   }

   enum fstVarDir dir = FST_VD_IMPLICIT;

   switch (tree_attr_int(d, fst_dir_i, -1)) {
   case PORT_IN: dir = FST_VD_INPUT; break;
   case PORT_OUT: dir = FST_VD_OUTPUT; break;
   case PORT_INOUT: dir = FST_VD_INOUT; break;
   case PORT_BUFFER: dir = FST_VD_BUFFER; break;
   }

   data->handle = fstWriterCreateVar2(
      fst_ctx,
      vt,
      dir,
      data->size,
      strrchr(istr(tree_ident(d)), ':') + 1,
      0,
      type_pp(type),
      FST_SVT_VHDL_SIGNAL,
      sdt);

   tree_add_attr_ptr(d, fst_data_i, data);

   data->watch = rt_set_event_cb(d, fst_event_cb);
}

static void fst_process_hier(tree_t h)
{
   const tree_kind_t scope_kind = tree_subkind(h);

   enum fstScopeType st;
   switch (scope_kind) {
   case T_ARCH: st = FST_ST_VHDL_ARCHITECTURE; break;
   case T_BLOCK: st = FST_ST_VHDL_BLOCK; break;
   case T_FOR_GENERATE: st = FST_ST_VHDL_FOR_GENERATE; break;
   case T_PACKAGE: st = FST_ST_VCD_PACKAGE; break;
   default:
      st = FST_ST_VHDL_ARCHITECTURE;
      warn_at(tree_loc(h), "no FST scope type for %s",
              tree_kind_str(scope_kind));
      break;
   }

   fstWriterSetScope(fst_ctx, st, istr(tree_ident(h)),
                     tree_has_ident2(h) ? istr(tree_ident2(h)) : "");
}

void fst_restart(void)
{
   if (fst_ctx == NULL)
      return;

   const int ndecls = tree_decls(fst_top);
   for (int i = 0; i < ndecls; i++) {
      tree_t d = tree_decl(fst_top, i);

      switch (tree_kind(d)) {
      case T_SIGNAL_DECL:
         fst_process_signal(d);
         break;
      case T_HIER:
         fst_process_hier(d);
         break;
      default:
         break;
      }

      int npop = tree_attr_int(d, ident_new("scope_pop"), 0);
      while (npop-- > 0)
         fstWriterSetUpscope(fst_ctx);
   }

   last_time = UINT64_MAX;

   for (int i = 0; i < ndecls; i++) {
      tree_t d = tree_decl(fst_top, i);
      if (tree_kind(d) == T_SIGNAL_DECL) {
         fst_data_t *data = tree_attr_ptr(d, fst_data_i);
         fst_event_cb(0, d, data->watch);
      }
   }
}

void fst_init(const char *file, tree_t top)
{
   fst_data_i   = ident_new("fst_data");
   std_logic_i  = ident_new("IEEE.STD_LOGIC_1164.STD_LOGIC");
   std_ulogic_i = ident_new("IEEE.STD_LOGIC_1164.STD_ULOGIC");
   std_bit_i    = ident_new("STD.STANDARD.BIT");
   fst_dir_i    = ident_new("fst_dir");
   std_bool_i   = ident_new("STD.STANDARD.BOOLEAN");
   std_char_i   = ident_new("STD.STANDARD.CHARACTER");
   natural_i    = ident_new("STD.STANDARD.NATURAL");
   positive_i   = ident_new("STD.STANDARD.POSITIVE");
   signed_i     = ident_new("IEEE.NUMERIC_STD.SIGNED");
   unsigned_i   = ident_new("IEEE.NUMERIC_STD.UNSIGNED");

   if ((fst_ctx = fstWriterCreate(file, 1)) == NULL)
      fatal("fstWriterCreate failed");

   fstWriterSetTimescale(fst_ctx, -15);

   atexit(fst_close);

   fst_top = top;
}
