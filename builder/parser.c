/* mgensys: Script parser.
 * Copyright (c) 2011, 2019-2020 Joel K. Pettersson
 * <joelkpettersson@gmail.com>.
 *
 * This file and the software of which it is part is distributed under the
 * terms of the GNU Lesser General Public License, either version 3 or (at
 * your option) any later version, WITHOUT ANY WARRANTY, not even of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * View the file COPYING for details, or if missing, see
 * <http://www.gnu.org/licenses/>.
 */

#include "../mgensys.h"
#include "../program.h"
#include "../math.h"
#include "../help.h"
#include "../loader/file.h"
#include "../mempool.h"
#include "../loader/symtab.h"
#include <string.h>
#include <stdlib.h>

#define IS_LOWER(c) ((c) >= 'a' && (c) <= 'z')
#define IS_UPPER(c) ((c) >= 'A' && (c) <= 'Z')
#define IS_ALPHA(c) (IS_LOWER(c) || IS_UPPER(c))
#define IS_DIGIT(c) ((c) >= '0' && (c) <= '9')
#define IS_ALNUM(c) (IS_ALPHA(c) || IS_DIGIT(c))
#define IS_SPACE(c) ((c) == ' ' || (c) == '\t')

/* Valid characters in identifiers. */
#define IS_SYMCHAR(c) (IS_ALNUM(c) || (c) == '_')

/* Sensible to print, for ASCII only. */
#define IS_VISIBLE(c) ((c) >= '!' && (c) <= '~')

static uint8_t filter_symchar(MGS_File *restrict f mgsMaybeUnused,
                              uint8_t c) {
  return IS_SYMCHAR(c) ? c : 0;
}

bool MGS_init_LangOpt(MGS_LangOpt *restrict o, MGS_SymTab *restrict symt) {
  o->wave_names = MGS_SymTab_pool_stra(symt, MGS_Wave_names,
      MGS_WAVE_TYPES);
  if (!o->wave_names)
    return false;
  return true;
}

typedef struct MGS_Parser {
  MGS_File *f;
  MGS_Program *prg;
  char *symbuf;
  uint32_t line;
  uint32_t reclevel;
  /* node state */
  uint32_t level;
  uint32_t setdef, setnode;
  MGS_ProgramNode *cur_node, *cur_root;
  MGS_ProgramNode *prev_node, *prev_root;
  MGS_ProgramNode *cur_dur;
  struct MGS_NodeData *cur_nd;
  MGS_SymStr *next_setsym;
  /* settings/ops */
  float n_pan;
  float n_ampmult;
  float n_time;
  float n_freq, n_ratio;
} MGS_Parser;

static mgsNoinline void warning(MGS_Parser *o, const char *s, char c) {
  MGS_File *f = o->f;
  if (IS_VISIBLE(c)) {
    fprintf(stderr, "warning: %s [line %d, at '%c'] - %s\n",
            f->path, o->line, c, s);
  } else if (c > MGS_FILE_MARKER) {
    fprintf(stderr, "warning: %s [line %d, at 0x%xc] - %s\n",
            f->path, o->line, c, s);
  } else if (MGS_File_AT_EOF(f)) {
    fprintf(stderr, "warning: %s [line %d, at EOF] - %s\n",
            f->path, o->line, s);
  } else {
    fprintf(stderr, "warning: %s [line %d] - %s\n",
            f->path, o->line, s);
  }
}

static void skip_ws(MGS_Parser *restrict o) {
  for (;;) {
    uint8_t c = MGS_File_GETC(o->f);
    if (IS_SPACE(c))
      continue;
    if (c == '\n') {
      ++o->line;
      MGS_File_TRYC(o->f, '\r');
    } else if (c == '\r') {
      ++o->line;
    } else if (c == '#') {
      MGS_File_skipline(o->f);
      c = MGS_File_GETC(o->f);
    } else {
      MGS_File_DECP(o->f);
      break;
    }
  }
}

/*
 * Handle unknown character, checking for EOF and treating
 * the character as invalid if not an end marker.
 *
 * \return false if EOF reached
 */
static bool check_invalid(MGS_Parser *restrict o, char c) {
  bool eof = MGS_File_AT_EOF(o->f);
  if (!eof || c > MGS_FILE_MARKER)
    warning(o, "invalid character", c);
  return !eof;
}

enum {
  ND_OWN_NODE = 1<<0,
};

/* things that need to be separate for each nested parse_level() go here */
typedef struct MGS_NodeData {
  MGS_Parser *o;
  struct MGS_NodeData *up;
  MGS_ProgramNode *node; /* state for tentative node until end_node() */
  MGS_ProgramArrData *target;
  MGS_SymStr *cur_setsym;
  uint32_t flags;
  /* timing/delay */
  uint8_t n_time_delay; // TODO: implement
  float n_delay_next;
} MGS_NodeData;

static void new_opdata(MGS_NodeData *nd) {
  MGS_Parser *o = nd->o;
  MGS_Program *p = o->prg;
  MGS_ProgramNode *n = nd->node;
  MGS_ProgramOpData *op;
  /*
   * Initial operator data.
   */
  if (!n->ref_prev) {
    op = MGS_MemPool_alloc(p->mem, sizeof(MGS_ProgramOpData));
    op->sound.amp = 1.f;
    op->sound.dynamp = op->sound.amp;
    op->sound.pan = o->n_pan;
    op->freq = o->n_freq;
    op->dynfreq = op->freq;
    n->base_id = p->base_counts[MGS_BASETYPE_SOUND]++;
  } else {
    MGS_ProgramNode *ref = n->ref_prev;
    op = MGS_MemPool_memdup(p->mem, ref->data, sizeof(MGS_ProgramOpData));
    op->sound.params = 0;
    n->base_id = ref->base_id;
  }
  /* time is not copied across reference */
  op->sound.time.v = o->n_time;
  op->sound.time.flags = 0;
  n->data = op;
}

static void new_durdata(MGS_NodeData *nd) {
  MGS_Parser *o = nd->o;
  MGS_Program *p = o->prg;
  MGS_ProgramNode *n = nd->node;
  MGS_ProgramDurData *dur;
  dur = MGS_MemPool_alloc(p->mem, sizeof(MGS_ProgramDurData));
  n->data = dur;
  if (o->cur_dur != NULL)
    o->cur_dur->use_next = n;
  o->cur_dur = n;
}

static void new_arrdata(MGS_NodeData *nd) {
  MGS_Parser *o = nd->o;
  MGS_Program *p = o->prg;
  MGS_ProgramNode *n = nd->node;
  MGS_ProgramArrData *arr;
  arr = MGS_MemPool_alloc(p->mem, sizeof(MGS_ProgramArrData));
  n->data = arr;
}

static void end_node(MGS_NodeData *nd);

static void new_node(MGS_NodeData *nd,
    MGS_ProgramNode *ref_prev, uint8_t type) {
  MGS_Parser *o = nd->o;
  MGS_Program *p = o->prg;
  end_node(nd);

  MGS_ProgramNode *n;
  n = MGS_MemPool_alloc(p->mem, sizeof(MGS_ProgramNode));
  nd->node = n;
  if (o->next_setsym != NULL) {
    nd->cur_setsym = o->next_setsym;
    o->next_setsym = NULL;
  }
  nd->flags |= ND_OWN_NODE;
  n->ref_prev = ref_prev;
  n->type = type;
  if (o->cur_dur != NULL && type != MGS_TYPE_DUR) {
    MGS_ProgramDurData *dur = o->cur_dur->data;
    if (!dur->scope.first_node)
      dur->scope.first_node = n;
    dur->scope.last_node = n;
  }

  /*
   * Handle IDs and linking.
   * References keep the original root ID
   * and are never added to nesting lists.
   * TODO: Implement references which copy
   * nodes to new locations, and/or move.
   */
  o->prev_node = o->cur_node;
  o->prev_root = o->cur_root;
  n->id = p->node_count;
  ++p->node_count;
  if (!p->node_list)
    p->node_list = n;
  else
    o->cur_node->next = n;
  o->cur_node = n;
  if (!ref_prev) {
    n->first_id = n->id;
    if (!nd->target) {
      n->root_id = n->first_id;
      ++p->root_count;
      o->cur_root = n;
    } else {
      n->root_id = o->cur_root->first_id;
      if (!nd->target->scope.first_node)
        nd->target->scope.first_node = n;
      else
        nd->target->scope.last_node->use_next = n;
      nd->target->scope.last_node = n;
      ++nd->target->count;
    }
  } else {
    n->first_id = ref_prev->first_id;
    n->root_id = ref_prev->root_id;
  }

  /* prepare timing adjustment */
  n->delay = nd->n_delay_next;
  nd->n_delay_next = 0.f;

  switch (type) {
  case MGS_TYPE_OP:
    new_opdata(nd);
    break;
  case MGS_TYPE_DUR:
    new_durdata(nd);
    break;
  case MGS_TYPE_ARR:
    new_arrdata(nd);
    break;
  }
}

static void end_opdata(MGS_NodeData *nd) {
  MGS_Parser *o = nd->o;
  MGS_ProgramNode *n = nd->node;
  MGS_ProgramOpData *op = n->data;
  /*
   * Prepare parsed operator data.
   */
  if (!n->ref_prev) {
    /* first node sets all values */
    op->sound.params |= MGS_PARAM_MASK & ~MGS_MODS_MASK;
  } else {
    if (op->sound.time.flags & MGS_TIME_SET)
      op->sound.params |= MGS_TIME;
  }
  if (op->sound.params & MGS_AMP) {
    if (n->first_id == n->root_id) /* only apply to non-modulators */
      op->sound.amp *= o->n_ampmult;
  }
}

static void end_durdata(MGS_NodeData *nd mgsMaybeUnused) {
}

static void end_arrdata(MGS_NodeData *nd mgsMaybeUnused) {
}

static void end_node(MGS_NodeData *nd) {
  MGS_ProgramNode *n = nd->node;
  if (!(nd->flags & ND_OWN_NODE))
    return; /* nothing to do */

  switch (n->type) {
  case MGS_TYPE_OP:
    end_opdata(nd);
    break;
  case MGS_TYPE_DUR:
    end_durdata(nd);
    break;
  case MGS_TYPE_ARR:
    end_arrdata(nd);
    break;
  }

  if (nd->cur_setsym) {
    nd->cur_setsym->data = n;
    nd->cur_setsym = NULL;
  }
  nd->node = NULL;
  nd->flags &= ~ND_OWN_NODE;
}

static void MGS_init_NodeData(MGS_NodeData *nd, MGS_Parser *o,
    MGS_ProgramArrData *target) {
  memset(nd, 0, sizeof(MGS_NodeData));
  nd->o = o;
  nd->target = target;
  if (o->cur_nd != NULL) {
    MGS_NodeData *up = o->cur_nd;
    nd->up = up;
    if (!target) {
      nd->target = up->target;
    }
  }
  if (!o->cur_dur) {
    new_node(nd, NULL, MGS_TYPE_DUR); // initial instance
  }
  o->cur_nd = nd;
}

static void MGS_fini_NodeData(MGS_NodeData *nd) {
  MGS_Parser *o = nd->o;
  end_node(nd);
  o->cur_nd = nd->up;
}

/* \return length if number read and \p val set */
typedef size_t (*NumSym_f)(MGS_Parser *restrict o, double *restrict val);

typedef struct NumParser {
  MGS_Parser *pr;
  NumSym_f numsym_f;
  bool has_infnum;
} NumParser;
enum {
  NUMEXP_SUB = 0,
  NUMEXP_ADT,
  NUMEXP_MLT,
  NUMEXP_POW,
  NUMEXP_NUM,
};
static double scan_num_r(NumParser *restrict o, uint8_t pri, uint32_t level) {
  MGS_Parser *pr = o->pr;
  double num;
  bool minus = false;
  uint8_t c;
  if (level > 0) skip_ws(pr);
  c = MGS_File_GETC(pr->f);
  if ((level > 0) && (c == '+' || c == '-')) {
    if (c == '-') minus = true;
    skip_ws(pr);
    c = MGS_File_GETC(pr->f);
  }
  if (c == '(') {
    num = scan_num_r(o, NUMEXP_SUB, level+1);
  } else if (o->numsym_f && IS_ALPHA(c)) {
    MGS_File_DECP(pr->f);
    size_t read_len = o->numsym_f(pr, &num);
    if (read_len == 0)
      return NAN;
    c = MGS_File_RETC(pr->f);
    if (IS_SYMCHAR(c)) {
      MGS_File_UNGETN(pr->f, read_len);
      return NAN;
    }
  } else {
    MGS_File_DECP(pr->f);
    size_t read_len;
    MGS_File_getd(pr->f, &num, false, &read_len);
    if (read_len == 0)
      return NAN;
  }
  if (minus) num = -num;
  if (level == 0 || pri == NUMEXP_NUM)
    return num; /* defer all */
  for (;;) {
    if (isinf(num)) o->has_infnum = true;
    if (level > 0) skip_ws(pr);
    c = MGS_File_GETC(pr->f);
    switch (c) {
    case '(':
      if (pri >= NUMEXP_MLT) goto DEFER;
      num *= scan_num_r(o, NUMEXP_SUB, level+1);
      break;
    case ')':
      if (pri != NUMEXP_SUB) goto DEFER;
      return num;
    case '^':
      if (pri >= NUMEXP_POW) goto DEFER;
      num = exp(log(num) * scan_num_r(o, NUMEXP_POW, level));
      break;
    case '*':
      if (pri >= NUMEXP_MLT) goto DEFER;
      num *= scan_num_r(o, NUMEXP_MLT, level);
      break;
    case '/':
      if (pri >= NUMEXP_MLT) goto DEFER;
      num /= scan_num_r(o, NUMEXP_MLT, level);
      break;
    case '+':
      if (pri >= NUMEXP_ADT) goto DEFER;
      num += scan_num_r(o, NUMEXP_ADT, level);
      break;
    case '-':
      if (pri >= NUMEXP_ADT) goto DEFER;
      num -= scan_num_r(o, NUMEXP_ADT, level);
      break;
    default:
      if (pri == NUMEXP_SUB) {
        warning(pr, "numerical expression has '(' without closing ')'", c);
      }
      goto DEFER;
    }
    if (isnan(num)) goto DEFER;
  }
DEFER:
  MGS_File_DECP(pr->f);
  return num;
}
static mgsNoinline bool scan_num(MGS_Parser *restrict o,
    NumSym_f scan_numsym, float *restrict var) {
  NumParser np = {o, scan_numsym, false};
  float num = scan_num_r(&np, NUMEXP_NUM, 0);
  if (isnan(num))
    return false;
  if (isinf(num)) np.has_infnum = true;
  if (np.has_infnum) {
    warning(o, "discarding expression with infinite number", 0);
    return false;
  }
  *var = num;
  return true;
}

static mgsNoinline bool scan_timeval(MGS_Parser *restrict o,
    float *restrict val) {
  float tval;
  if (!scan_num(o, NULL, &tval))
    return false;
  if (tval < 0) {
    warning(o, "discarding negative time value", 0);
    return false;
  }
  *val = tval;
  return true;
}

#define SYMKEY_MAXLEN 79
#define SYMKEY_MAXLEN_A "79"
static bool scan_sym(MGS_Parser *o, MGS_SymStr **sym, char pos_c) {
  size_t len = 0;
  bool truncated;
  truncated = !MGS_File_getstr(o->f, o->symbuf, SYMKEY_MAXLEN + 1,
      &len, filter_symchar);
  if (len == 0) {
    warning(o, "symbol name missing", pos_c);
    return false;
  }
  *sym = MGS_SymTab_get_symstr(o->prg->symt, o->symbuf, len);
  if (truncated) {
    warning(o, "limiting symbol name to "SYMKEY_MAXLEN_A" characters", pos_c);
    len += MGS_File_skipstr(o->f, filter_symchar);
  }
  return true;
}

static bool scan_symafind(MGS_Parser *restrict o,
                          const char *const*restrict stra,
                          size_t *restrict found_i, char pos_c) {
  size_t len = 0;
  bool truncated = !MGS_File_getstr(o->f, o->symbuf, SYMKEY_MAXLEN + 1,
      &len, filter_symchar);
  if (len == 0) {
    warning(o, "named value missing", pos_c);
    return false;
  }
  const char *key = MGS_SymTab_pool_str(o->prg->symt, o->symbuf, len);
  if (truncated) {
    warning(o, "limiting named value to "SYMKEY_MAXLEN_A" characters", pos_c);
    len += MGS_File_skipstr(o->f, filter_symchar);
  }
  for (size_t i = 0; stra[i] != NULL; ++i) {
    if (stra[i] == key) {
      *found_i = i;
      return true;
    }
  }
  return false;
}

static size_t numsym_channel(MGS_Parser *restrict o, double *restrict val) {
  char c = MGS_File_GETC(o->f);
  switch (c) {
  case 'C':
    *val = 0.f;
    return 1;
  case 'L':
    *val = -1.f;
    return 1;
  case 'R':
    *val = 1.f;
    return 1;
  default:
    MGS_File_DECP(o->f);
    return 0;
  }
}

static bool scan_wavetype(MGS_Parser *restrict o,
    size_t *restrict found_id, char pos_c) {
  const char *const *names = o->prg->lopt.wave_names;
  if (scan_symafind(o, names, found_id, pos_c))
    return true;
  warning(o, "invalid wave type value; available are:", pos_c);
  MGS_print_names(names, "\t", stderr);
  return false;
}

static void parse_level(MGS_Parser *o, MGS_ProgramArrData *chain);

static MGS_Program* parse(MGS_File *f, MGS_Parser *o) {
  memset(o, 0, sizeof(MGS_Parser));
  o->f = f;
  MGS_MemPool *mem = MGS_create_MemPool(0);
  MGS_SymTab *symt = MGS_create_SymTab(mem);
  o->prg = MGS_MemPool_alloc(mem, sizeof(MGS_Program));
  o->prg->mem = mem;
  o->prg->symt = symt;
  o->prg->name = f->path;
  MGS_init_LangOpt(&o->prg->lopt, symt);
  o->symbuf = calloc(SYMKEY_MAXLEN + 1, sizeof(char));
  o->line = 1;
  o->n_pan = 0.f; /* default until changed */
  o->n_ampmult = 1.f; /* default until changed */
  o->n_time = 1.f; /* default until changed */
  o->n_freq = 100.f; /* default until changed */
  o->n_ratio = 1.f; /* default until changed */
  parse_level(o, NULL);
  free(o->symbuf);
  return o->prg;
}

static bool parse_amp(MGS_Parser *o, MGS_ProgramNode *n) {
  MGS_Program *p = o->prg;
  MGS_ProgramSoundData *sound;
  sound = MGS_ProgramNode_get_data(n, MGS_BASETYPE_SOUND);
  if (!sound) goto INVALID;
  MGS_NodeData *nd = o->cur_nd;
  if (nd->target != NULL) {
    MGS_ProgramArrData *target = nd->target;
    if (target->mod_type == MGS_AMODS ||
        target->mod_type == MGS_FMODS)
      goto INVALID;
  }
  float f;
  if (MGS_File_TRYC(o->f, '!')) {
    if (!MGS_File_TESTC(o->f, '{')) {
      if (!scan_num(o, NULL, &f)) goto INVALID;
      sound->dynamp = f;
      sound->params |= MGS_DYNAMP;
    }
    if (MGS_File_TRYC(o->f, '{')) {
      sound->amod = MGS_MemPool_alloc(p->mem, sizeof(MGS_ProgramArrData));
      sound->amod->mod_type = MGS_AMODS;
      parse_level(o, sound->amod);
      sound->params |= MGS_AMODS;
    }
  } else {
    if (!scan_num(o, NULL, &f)) goto INVALID;
    sound->amp = f;
    sound->params |= MGS_AMP;
  }
  return true;
INVALID:
  return false;
}

static bool parse_channel(MGS_Parser *o, MGS_ProgramNode *n) {
  MGS_ProgramSoundData *sound;
  sound = MGS_ProgramNode_get_data(n, MGS_BASETYPE_SOUND);
  if (!sound) goto INVALID;
  MGS_NodeData *nd = o->cur_nd;
  if (nd->target != NULL) {
    MGS_ProgramArrData *target = nd->target;
    if (target->mod_type != 0) goto INVALID;
  }
  float f;
  /* TODO: support modulation */
  if (!scan_num(o, numsym_channel, &f)) goto INVALID;
  sound->pan = f;
  sound->params |= MGS_PAN;
  return true;
INVALID:
  return false;
}

static bool parse_freq(MGS_Parser *o, MGS_ProgramNode *n, bool ratio) {
  MGS_Program *p = o->prg;
  MGS_ProgramOpData *op = MGS_ProgramNode_get_data(n, MGS_TYPE_OP);
  if (!op) goto INVALID;
  if (ratio && (n->first_id == n->root_id)) goto INVALID;
  float f;
  if (MGS_File_TRYC(o->f, '!')) {
    if (!MGS_File_TESTC(o->f, '{')) {
      if (!scan_num(o, NULL, &f)) goto INVALID;
      if (ratio) {
        op->dynfreq = 1.f / f;
        op->attr |= MGS_ATTR_DYNFREQRATIO;
      } else {
        op->dynfreq = f;
        op->attr &= ~MGS_ATTR_DYNFREQRATIO;
      }
      op->sound.params |= MGS_DYNFREQ | MGS_ATTR;
    }
    if (MGS_File_TRYC(o->f, '{')) {
      op->fmod = MGS_MemPool_alloc(p->mem, sizeof(MGS_ProgramArrData));
      op->fmod->mod_type = MGS_FMODS;
      parse_level(o, op->fmod);
      op->sound.params |= MGS_FMODS;
    }
  } else {
    if (!scan_num(o, NULL, &f)) goto INVALID;
    if (ratio) {
      op->freq = 1.f / f;
      op->attr |= MGS_ATTR_FREQRATIO;
    } else {
      op->freq = f;
      op->attr &= ~MGS_ATTR_FREQRATIO;
    }
    op->sound.params |= MGS_FREQ | MGS_ATTR;
  }
  return true;
INVALID:
  return false;
}

static bool parse_phase(MGS_Parser *o, MGS_ProgramNode *n) {
  MGS_Program *p = o->prg;
  MGS_ProgramOpData *op = MGS_ProgramNode_get_data(n, MGS_TYPE_OP);
  if (!op) goto INVALID;
  float f;
  if (MGS_File_TRYC(o->f, '!')) {
    if (MGS_File_TRYC(o->f, '{')) {
      op->pmod = MGS_MemPool_alloc(p->mem, sizeof(MGS_ProgramArrData));
      op->pmod->mod_type = MGS_PMODS;
      parse_level(o, op->pmod);
      op->sound.params |= MGS_PMODS;
    }
  } else {
    if (!scan_num(o, NULL, &f)) goto INVALID;
    op->phase = fmod(f, 1.f);
    if (op->phase < 0.f)
      op->phase += 1.f;
    op->sound.params |= MGS_PHASE;
  }
  return true;
INVALID:
  return false;
}

static bool parse_time(MGS_Parser *o, MGS_ProgramNode *n) {
  MGS_ProgramSoundData *sound;
  sound = MGS_ProgramNode_get_data(n, MGS_BASETYPE_SOUND);
  if (!sound) goto INVALID;
  float f;
  if (!scan_timeval(o, &f)) goto INVALID;
  sound->time.v = f;
  sound->time.flags |= MGS_TIME_SET;
  return true;
INVALID:
  return false;
}

static bool parse_wave(MGS_Parser *o, MGS_ProgramNode *n, char pos_c) {
  MGS_ProgramOpData *op = MGS_ProgramNode_get_data(n, MGS_TYPE_OP);
  if (!op) goto INVALID;
  size_t wave;
  if (!scan_wavetype(o, &wave, pos_c)) goto INVALID;
  op->wave = wave;
  op->sound.params |= MGS_WAVE;
  return true;
INVALID:
  return false;
}

static bool parse_ref(MGS_Parser *o, char pos_c) {
  MGS_NodeData *nd = o->cur_nd;
  if (nd->target != NULL)
    return false;
  MGS_SymStr *sym = NULL;
  if (!scan_sym(o, &sym, ':'))
    return false;
  if (sym != NULL) {
    MGS_ProgramNode *ref = sym->data;
    if (!ref) {
      warning(o, "ignoring reference to undefined label", pos_c);
      return true;
    }
    o->next_setsym = sym;
    new_node(nd, ref, ref->type);
    o->setnode = o->level + 1;
  }
  return true;
}

static void parse_level(MGS_Parser *o, MGS_ProgramArrData *chain) {
  char c;
  float f;
  MGS_NodeData nd;
  uint32_t entrylevel = o->level;
  ++o->reclevel;
  MGS_init_NodeData(&nd, o, chain);
  for (;;) {
    c = MGS_File_GETC(o->f);
    MGS_File_skipspace(o->f);
    switch (c) {
    case '\n':
      MGS_File_TRYC(o->f, '\r');
      /* fall-through */
    case '\r':
      if (!nd.target) {
        if (o->setdef > o->level)
          o->setdef = (o->level) ? (o->level - 1) : 0;
        else if (o->setnode > o->level) {
          o->setnode = (o->level) ? (o->level - 1) : 0;
          end_node(&nd);
        }
      }
      ++o->line;
      break;
    case '\t':
    case ' ':
      MGS_File_skipspace(o->f);
      break;
    case '#':
      MGS_File_skipline(o->f);
      break;
    case '/':
      if (o->setdef > o->setnode) goto INVALID;
      if (MGS_File_TRYC(o->f, 't')) {
        nd.n_time_delay = 1;
        break;
      }
      if (!scan_timeval(o, &f)) goto INVALID;
      nd.n_time_delay = 0;
      nd.n_delay_next += f;
      break;
    case '{':
      /* is always got elsewhere before a nesting call to this function */
      warning(o, "opening curly brace out of place", c);
      break;
    case '}':
      if (!nd.target)
        goto INVALID;
      if (o->level != entrylevel) {
        o->level = entrylevel;
        warning(o, "closing '}' before closing '>'s", c);
      }
      goto RETURN;
    case '<':
      ++o->level;
      break;
    case '>':
      if (!o->level) {
        warning(o, "closing '>' without opening '<'", c);
        break;
      }
      if (o->setdef > o->level)
        o->setdef = (o->level) ? (o->level - 1) : 0;
      else if (o->setnode > o->level) {
        o->setnode = (o->level) ? (o->level - 1) : 0;
        end_node(&nd);
      }
      --o->level;
      break;
    case 'E':
      new_node(&nd, NULL, MGS_TYPE_ENV);
      o->setnode = o->level + 1;
      break;
    case 'Q':
      goto FINISH;
    case 'S':
      o->setdef = o->level + 1;
      break;
    case 'W': {
      size_t wave;
      if (!scan_wavetype(o, &wave, c)) break;
      new_node(&nd, NULL, MGS_TYPE_OP);
      MGS_ProgramOpData *op = nd.node->data;
      op->wave = wave;
      o->setnode = o->level + 1;
      break; }
    case '|': {
      MGS_ProgramDurData *dur = o->cur_dur->data;
      if (!dur->scope.first_node) {
        warning(o, "no sounds precede time separator", c);
        break;
      }
      new_node(&nd, NULL, MGS_TYPE_DUR);
      break; }
    case '\\':
      if (o->setdef > o->setnode)
        goto INVALID;
      if (!scan_timeval(o, &f)) goto INVALID;
      nd.node->delay += f;
      break;
    case '\'':
      end_node(&nd);
      if (o->next_setsym != NULL) {
        warning(o, "ignoring label assignment to label assignment", c);
        break;
      }
      if (!scan_sym(o, &o->next_setsym, '\'')) goto INVALID;
      break;
    case ':':
      end_node(&nd);
      if (o->next_setsym != NULL) {
        warning(o, "ignoring label assignment to label reference", c);
        // continue parsing reference use
      }
      if (!parse_ref(o, c)) goto INVALID;
      break;
    case 'a':
      if (o->setdef > o->setnode) {
        if (!scan_num(o, NULL, &f)) goto INVALID;
        o->n_ampmult = f;
        break;
      } else if (o->setnode <= 0)
        goto INVALID;
      if (!parse_amp(o, nd.node)) goto INVALID;
      break;
    case 'c':
      if (o->setdef > o->setnode) {
        if (!scan_num(o, numsym_channel, &f)) goto INVALID;
        o->n_pan = f;
        break;
      } else if (o->setnode <= 0)
        goto INVALID;
      if (!parse_channel(o, nd.node)) goto INVALID;
      break;
    case 'f':
      if (o->setdef > o->setnode) {
        if (!scan_num(o, NULL, &f)) goto INVALID;
        o->n_freq = f;
        break;
      } else if (o->setnode <= 0)
        goto INVALID;
      if (!parse_freq(o, nd.node, false)) goto INVALID;
      break;
    case 'p':
      if (o->setdef > o->setnode || o->setnode <= 0)
        goto INVALID;
      if (!parse_phase(o, nd.node)) goto INVALID;
      break;
    case 'r':
      if (o->setdef > o->setnode) {
        if (!scan_num(o, NULL, &f)) goto INVALID;
        o->n_ratio = 1.f / f;
        break;
      } else if (o->setnode <= 0)
        goto INVALID;
      if (!parse_freq(o, nd.node, true)) goto INVALID;
      break;
    case 't':
      if (o->setdef > o->setnode) {
        if (!scan_timeval(o, &f)) goto INVALID;
        o->n_time = f;
        break;
      } else if (o->setnode <= 0)
        goto INVALID;
      if (!parse_time(o, nd.node)) goto INVALID;
      break;
    case 'w':
      if (o->setdef > o->setnode || o->setnode <= 0)
        goto INVALID;
      if (!parse_wave(o, nd.node, c)) goto INVALID;
      break;
    default:
    INVALID:
      if (!check_invalid(o, c)) goto FINISH;
      break;
    }
  }
FINISH:
  if (o->level)
    warning(o, "end of file without closing '>'s", c);
  if (o->reclevel > 1)
    warning(o, "end of file without closing '}'s", c);
RETURN:
  MGS_fini_NodeData(&nd);
  --o->reclevel;
}

static void time_sound(MGS_ProgramNode *n, MGS_ProgramSoundData *sound) {
  if (!(sound->time.flags & MGS_TIME_SET)) {
    if (n->first_id != n->root_id)
      sound->time.flags |= MGS_TIME_SET;
  }
  // handle timing for sub-components here
  // handle timing for added silence here
}

/*
 * Adjust timing for a duration scope; the syntax for such time grouping is
 * only allowed on the top scope, so the algorithm only deals with this for
 * the nodes involved.
 */
static void time_durscope(MGS_ProgramDurData *dur) {
  MGS_ProgramNode *n_after = dur->scope.last_node->next;
  double delay = 0.f, delaycount = 0.f;
  MGS_ProgramNode *step;
  for (step = dur->scope.first_node; step != n_after; ) {
    if (step->first_id != step->root_id) {
      /* skip this node; nested nodes are excluded from duration */
      step = step->next;
      continue;
    }
    MGS_ProgramSoundData *sound;
    sound = MGS_ProgramNode_get_data(step, MGS_BASETYPE_SOUND);
    if (!sound) continue; /* skip unsupported node */
    if (step->next == n_after) {
      /* accept pre-set default time for last node in group */
      sound->time.flags |= MGS_TIME_SET;
    }
    if (delay < sound->time.v)
      delay = sound->time.v;
    step = step->next;
    if (step != NULL) {
      delaycount += step->delay;
    }
  }
  for (step = dur->scope.first_node; step != n_after; ) {
    if (step->first_id != step->root_id) {
      /* skip this node; nested nodes are excluded from duration */
      step = step->next;
      continue;
    }
    MGS_ProgramSoundData *sound;
    sound = MGS_ProgramNode_get_data(step, MGS_BASETYPE_SOUND);
    if (!sound) continue; /* skip unsupported node */
    if (!(sound->time.flags & MGS_TIME_SET)) {
      sound->time.v = delay + delaycount; /* fill in sensible default time */
      sound->time.flags |= MGS_TIME_SET;
    }
    step = step->next;
    if (step != NULL) {
      delaycount -= step->delay;
    }
  }
  if (n_after != NULL)
    n_after->delay += delay;
}

static void adjust_nodes(MGS_ProgramNode *list) {
  MGS_ProgramNode *n = list;
  MGS_ProgramDurData *dur = NULL;
  while (n != NULL) {
    if (n->type == MGS_TYPE_DUR) {
      dur = n->data;
      n = n->next;
      continue;
    }
    MGS_ProgramSoundData *sound;
    sound = MGS_ProgramNode_get_data(n, MGS_BASETYPE_SOUND);
    if (sound != NULL) time_sound(n, sound);
    if (n == dur->scope.last_node) time_durscope(dur);
    n = n->next;
  }
}

MGS_Program* MGS_create_Program(const char *file, bool is_path) {
  MGS_Program *o = NULL;
  MGS_Parser p;
  MGS_File *f = MGS_create_File();
  if (!f) goto ERROR;
  if (!is_path) {
    if (!MGS_File_stropenrb(f, "<string>", file)) {
      MGS_error(NULL, "NULL string passed for opening");
      goto ERROR;
    }
  } else if (!MGS_File_fopenrb(f, file)) {
    MGS_error(NULL, "couldn't open script file \"%s\" for reading", file);
    goto ERROR;
  }
  o = parse(f, &p);
  if (!o) goto ERROR;
  adjust_nodes(o->node_list);
ERROR:
  MGS_destroy_File(f);
  return o;
}

void MGS_destroy_Program(MGS_Program *o) {
  if (!o)
    return;
  MGS_destroy_SymTab(o->symt);
  MGS_destroy_MemPool(o->mem);
}
