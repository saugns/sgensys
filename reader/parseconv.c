/* saugns: Parser output to script data converter.
 * Copyright (c) 2011-2012, 2017-2021 Joel K. Pettersson
 * <joelkpettersson@gmail.com>.
 *
 * This file and the software of which it is part is distributed under the
 * terms of the GNU Lesser General Public License, either version 3 or (at
 * your option) any later version, WITHOUT ANY WARRANTY, not even of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * View the file COPYING for details, or if missing, see
 * <https://www.gnu.org/licenses/>.
 */

#include "parser.h"

/*
 * Script data construction from parse data.
 *
 * Adjust and replace data structures. The per-event
 * operator list becomes flat, with separate lists kept for
 * recursive traversal in scriptconv.
 */

/*
 * Deals with events that are "composite" (attached to a main event as
 * successive "sub-events" rather than part of the big, linear event sequence).
 *
 * Such events, if attached to the passed event, will be given their place in
 * the ordinary event list.
 */
static void flatten_events(SAU_ParseEvent *restrict e) {
	SAU_ParseEvent *ce = e->composite;
	SAU_ParseEvent *se = e->next, *se_prev = e;
	uint32_t wait_ms = 0;
	uint32_t added_wait_ms = 0;
	while (ce != NULL) {
		if (!se) {
			/*
			 * No more events in the ordinary sequence,
			 * so append all composites.
			 */
			se_prev->next = ce;
			break;
		}
		/*
		 * If several events should pass in the ordinary sequence
		 * before the next composite is inserted, skip ahead.
		 */
		wait_ms += se->wait_ms;
		if (se->next && (wait_ms + se->next->wait_ms)
				<= (ce->wait_ms + added_wait_ms)) {
			se_prev = se;
			se = se->next;
			continue;
		}
		/*
		 * Insert next composite before or after
		 * the next event of the ordinary sequence.
		 */
		SAU_ParseEvent *ce_next = ce->next;
		if (se->wait_ms >= (ce->wait_ms + added_wait_ms)) {
			se->wait_ms -= ce->wait_ms + added_wait_ms;
			added_wait_ms = 0;
			wait_ms = 0;
			se_prev->next = ce;
			se_prev = ce;
			se_prev->next = se;
		} else {
			SAU_ParseEvent *se_next = se->next;
			ce->wait_ms -= wait_ms;
			added_wait_ms += ce->wait_ms;
			wait_ms = 0;
			se->next = ce;
			ce->next = se_next;
			se_prev = ce;
			se = se_next;
		}
		ce = ce_next;
	}
	e->composite = NULL;
}

typedef struct ParseConv {
	SAU_ScriptEvData *ev, *first_ev;
	SAU_MemPool *mem, *tmp;
} ParseConv;

/*
 * Per-operator data pointed to by all its nodes during conversion.
 */
typedef struct OpContext {
	SAU_ParseOpData *last_use;
} OpContext;

/*
 * Get operator context for node, updating associated data.
 *
 * If the node is ignored, the SAU_PDOP_IGNORED flag is set
 * before returning NULL. A NULL context means either error
 * or an ignored node.
 *
 * \return instance, or NULL on allocation failure or ignored node
 */
static OpContext *ParseConv_update_opcontext(ParseConv *restrict o,
		SAU_ScriptOpData *restrict od,
		SAU_ParseOpData *restrict pod) {
	OpContext *oc = NULL;
	SAU_ParseOpData *pod_old = pod->ref.old;
	SAU_ScriptEvData *e = o->ev;
	if (!pod_old) {
		oc = SAU_MemPool_alloc(o->tmp, sizeof(OpContext));
		if (!oc)
			return NULL;
		if (od->use_type == SAU_POP_CARR) {
			e->ev_flags |= SAU_SDEV_NEW_OPGRAPH;
			od->op_flags |= SAU_SDOP_ADD_CARRIER;
		}
	} else {
		oc = pod_old->op_context;
		if (!oc) {
			/*
			 * This can happen if earlier nodes were excluded,
			 * in which case all follow-ons nodes will also be
			 * ignored.
			 */
			pod->op_flags |= SAU_PDOP_IGNORED;
			return NULL;
		}
		if (od->use_type == SAU_POP_CARR) {
			od->op_flags |= SAU_SDOP_ADD_CARRIER;
		}
		SAU_ScriptOpData *prev_use = oc->last_use->op_conv;
		od->prev_use = prev_use;
		prev_use->next_use = od;
		prev_use->event->ev_flags |= SAU_SDEV_LATER_USED;
		e->root_ev = od->root_event;
	}
	oc->last_use = pod;
	pod->op_context = oc;
	return oc;
}

/*
 * Convert data for an operator node to script operator data,
 * adding it to the list to be used for the current script event.
 */
static bool ParseConv_add_opdata(ParseConv *restrict o,
		SAU_ParseOpData *restrict pod) {
	SAU_ScriptOpData *od = SAU_MemPool_alloc(o->mem,
			sizeof(SAU_ScriptOpData));
	if (!od) goto ERROR;
	SAU_ScriptEvData *e = o->ev;
	pod->op_conv = od;
	od->root_event = pod->root_event->ev_conv;
	od->event = e;
	/* ref.next_item */
	/* op_flags */
	od->params = pod->params;
	od->time = pod->time;
	od->silence_ms = pod->silence_ms;
	od->wave = pod->wave;
	od->use_type = pod->use_type;
	od->freq = pod->freq;
	od->freq2 = pod->freq2;
	od->amp = pod->amp;
	od->amp2 = pod->amp2;
	od->pan = pod->pan;
	od->phase = pod->phase;
	if (!ParseConv_update_opcontext(o, od, pod)) goto ERROR;
	if (!e->op_all.first)
		e->op_all.first = od;
	else
		((SAU_ScriptOpData*) e->op_all.last)->range_next = od;
	e->op_all.last = od;
	return true;
ERROR:
	return false;
}

/*
 * Recursively create needed nodes for part of parse.
 */
static bool ParseConv_add_nodes(ParseConv *restrict o,
		const SAU_NodeRange *restrict pod_list) {
	if (!pod_list)
		return true;
	SAU_ParseOpData *pod = pod_list->first;
	for (; pod != NULL; pod = pod->ref.next_item) {
		// TODO: handle multiple operator nodes
		if (pod->op_flags & SAU_PDOP_MULTIPLE) {
			// TODO: handle multiple operator nodes
			pod->op_flags |= SAU_PDOP_IGNORED;
			continue;
		}
		if (!ParseConv_add_opdata(o, pod)) {
			if (pod->op_flags & SAU_PDOP_IGNORED) continue;
			goto ERROR;
		}
		for (SAU_ParseSublist *scope = pod->ref.sublists;
				scope != NULL; scope = scope->next) {
			if (!ParseConv_add_nodes(o, &scope->range)) goto ERROR;
		}
	}
	return true;
ERROR:
	return false;
}

/*
 * Recursively fill in lists for conversion of part of parse after nodes made.
 */
static bool ParseConv_link_nodes(ParseConv *restrict o,
		SAU_RefList *restrict *od_list,
		const SAU_NodeRange *restrict pod_list,
		uint8_t list_type) {
	if (!pod_list)
		return true;
	SAU_ScriptEvData *e = o->ev;
	if (list_type != SAU_POP_CARR ||
			(e->ev_flags & SAU_SDEV_NEW_OPGRAPH) != 0) {
		*od_list = SAU_create_RefList(list_type, o->mem);
		if (!*od_list) goto ERROR;
	}
	SAU_ParseOpData *pod = pod_list->first;
	for (; pod != NULL; pod = pod->ref.next_item) {
		if (pod->op_flags & SAU_PDOP_IGNORED) continue;
		SAU_ScriptOpData *od = pod->op_conv;
		if (!od) goto ERROR;
		if ((list_type != SAU_POP_CARR ||
				 ((e->ev_flags & SAU_SDEV_NEW_OPGRAPH) &&
				  (od->op_flags & SAU_SDOP_ADD_CARRIER))) &&
				!SAU_RefList_add(*od_list, od, 0, o->mem))
			goto ERROR;
		SAU_RefList *last_mod_list = NULL;
		for (SAU_ParseSublist *scope = pod->ref.sublists;
				scope != NULL; scope = scope->next) {
			SAU_RefList *next_mod_list = NULL;
			if (!ParseConv_link_nodes(o, &next_mod_list,
						&scope->range,
						scope->use_type)) goto ERROR;
			if (!od->mod_lists)
				od->mod_lists = next_mod_list;
			else
				last_mod_list->next = next_mod_list;
			last_mod_list = next_mod_list;
		}
	}
	return true;
ERROR:
	return false;
}

/*
 * Convert the given event data node and all associated operator data nodes.
 */
static bool ParseConv_add_event(ParseConv *restrict o,
		SAU_ParseEvent *restrict pe) {
	SAU_ScriptEvData *e = SAU_MemPool_alloc(o->mem,
			sizeof(SAU_ScriptEvData));
	if (!e) goto ERROR;
	pe->ev_conv = e;
	if (!o->first_ev)
		o->first_ev = e;
	else
		o->ev->next = e;
	o->ev = e;
	e->wait_ms = pe->wait_ms;
	/* ev_flags */
	const SAU_NodeRange ev_op = {.first = pe->op_data};
	if (!ParseConv_add_nodes(o, &ev_op)) goto ERROR;
	if (!ParseConv_link_nodes(o, &e->carriers,
				&ev_op, SAU_POP_CARR)) goto ERROR;
	return true;
ERROR:
	return false;
}

/*
 * Convert parser output to script data, performing
 * post-parsing passes. Perform timing adjustments,
 * flatten event list.
 *
 * Ideally, adjustments of parse data would be
 * more cleanly separated into the later stages.
 */
static SAU_Script *ParseConv_convert(ParseConv *restrict o,
		SAU_Parse *restrict p) {
	o->mem = SAU_create_MemPool(0);
	o->tmp = p->mem;
	if (!o->mem || !o->tmp) goto ERROR;
	SAU_Script *s = SAU_MemPool_alloc(o->mem, sizeof(SAU_Script));
	if (!s) goto ERROR;
	s->name = p->name;
	s->sopt = p->sopt;
	s->mem = o->mem;
	/*
	 * Convert events, flattening the remaining list while proceeding.
	 * Flattening must be done following the timing adjustment pass;
	 * otherwise, cannot always arrange events in the correct order.
	 */
	for (SAU_ParseEvent *pe = p->events; pe != NULL; pe = pe->next) {
		if (!ParseConv_add_event(o, pe)) goto ERROR;
		if (pe->composite != NULL) flatten_events(pe);
	}
	s->events = o->first_ev;
	if (false)
	ERROR: {
		SAU_destroy_MemPool(o->mem);
		SAU_error("parseconv", "memory allocation failure");
		s = NULL;
	}
	return s;
}

/**
 * Create script data for the given script. Invokes the parser.
 *
 * \return instance or NULL on error
 */
SAU_Script *SAU_load_Script(const char *restrict script_arg, bool is_path) {
	ParseConv pc = (ParseConv){0};
	SAU_Parse *p = SAU_create_Parse(script_arg, is_path);
	if (!p)
		return NULL;
	SAU_Script *o = ParseConv_convert(&pc, p);
	SAU_destroy_Parse(p);
	return o;
}

/**
 * Destroy script data.
 */
void SAU_discard_Script(SAU_Script *restrict o) {
	if (!o)
		return;
	SAU_destroy_MemPool(o->mem);
}
