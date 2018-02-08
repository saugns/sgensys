/* sgensys: Symbol table module.
 * Copyright (c) 2011-2012, 2017-2018 Joel K. Pettersson
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

#pragma once
#include "sgensys.h"

struct SGSSymTab;
typedef struct SGSSymTab *SGSSymTab_t;

SGSSymTab_t SGS_create_symtab(void);
void SGS_destroy_symtab(SGSSymTab_t o);

void* SGS_symtab_get(SGSSymTab_t o, const char *key);
void* SGS_symtab_set(SGSSymTab_t o, const char *key, void *value);
