/* sgensys sound program interpreter
 * Copyright (c) 2013-2014, 2018 Joel K. Pettersson
 * <joelkpettersson@gmail.com>.
 *
 * This file and the software of which it is part is distributed under the
 * terms of the GNU Lesser General Public License, either version 3 or (at
 * your option) any later version; WITHOUT ANY WARRANTY, not even of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * View the file COPYING for details, or if missing, see
 * <http://www.gnu.org/licenses/>.
 */

#include "sgensys.h"
#include "program.h"
#include "interpreter.h"
#include <stdlib.h>

struct SGSInterpreter {
	SGSSoundData *results;
	size_t result_count,
	       result_alloc;
};

typedef struct SGSInterpreter SGSInterpreter;

struct SGSInterpreter *SGS_create_interpreter() {
	SGSInterpreter *o = calloc(1, sizeof(SGSInterpreter));
	return o;
}

void SGS_destroy_interpreter(struct SGSInterpreter *o) {
	SGS_interpreter_clear(o);
	free(o);
}

SGSSoundData *SGS_interpreter_run(struct SGSInterpreter *o,
                                  SGSProgram *program) {
	SGSSoundData *result = run_program(SGSProgram *program);
	if (result == NULL) {
		return NULL;
	}

	if (o->result_count == o->result_alloc) {
		if (o->result_alloc == 0) {
			o->result_alloc = 1;
		} else {
			o->result_alloc <<= 1;
		}
		o->results = realloc(o->results,
		                     o->result_alloc * sizeof(SGSSoundData));
	}
	o->results[o->result_count] = result;
	++o->result_count;

	return result;
}

void SGS_interpreter_clear(struct SGSInterpreter *o) {
	free(o->results);
}

#if 0 /* old generator.c code */

/*
 *
 * Count buffers needed for operator, including linked operators.
 * TODO: Verify, remove debug printing when parser module done.
 */
static uint calc_blocks(SGSGenerator *o, OperatorNode *n) {
	uint count = 0, i, res;
	if (n->adjcs) {
		const int *mods = n->adjcs->adjcs;
		const uint modc = n->adjcs->fmodc + n->adjcs->pmodc + n->adjcs->amodc;
		for (i = 0; i < modc; ++i) {
	printf("visit node %d\n", mods[i]);
			res = calc_blocks(o, &o->operators[mods[i]]);
			if (res > count) count = res;
		}
	}
	return count + 5;
}

/*
 * Check operators for voice and increase the buffer allocation if needed.
 * TODO: Verify, remove debug printing when parser module done.
 */
static void upsize_blocks(SGSGenerator *o, VoiceNode *vn) {
	uint count = 0, i, res;
	if (!vn->graph) return;
	for (i = 0; i < vn->graph->opc; ++i) {
	printf("visit node %d\n", vn->graph->ops[i]);
		res = calc_blocks(o, &o->operators[vn->graph->ops[i]]);
		if (res > count) count = res;
	}
	printf("need %d buffers (have %d)\n", count, o->blockc);
	if (count > o->blockc) {
		printf("new alloc size 0x%x\n", sizeof(Buf) * count);
		o->blocks = realloc(o->blocks, sizeof(Buf) * count);
		o->blockc = count;
	}
}

/*
 * Generate up to buf_len samples for an operator node, the remainder (if any)
 * zero-filled if acc_ind is zero.
 *
 * Recursively visits the subnodes of the operator node in the process, if
 * any.
 *
 * Returns number of samples generated for the node.
 */
static uint run_block(SGSGenerator *o, Buf *blocks, uint buf_len,
		OperatorNode *n, BufData *parent_freq, uchar wave_env,
		uint acc_ind) {
	uint i, len, zero_len, skip_len;
	BufData *sbuf, *freq, *freqmod, *pm, *amp;
	Buf *nextbuf = blocks + 1;
	ParameterValit *vi;
	uchar fmodc, pmodc, amodc;
	fmodc = pmodc = amodc = 0;
	if (n->adjcs) {
		fmodc = n->adjcs->fmodc;
		pmodc = n->adjcs->pmodc;
		amodc = n->adjcs->amodc;
	}
	sbuf = *blocks;
	len = buf_len;
	/*
	 * If silence, zero-fill and delay processing for duration.
	 */
	zero_len = 0;
	if (n->silence) {
		zero_len = n->silence;
		if (zero_len > len)
			zero_len = len;
		if (!acc_ind) for (i = 0; i < zero_len; ++i)
			sbuf[i].i = 0;
		len -= zero_len;
		if (n->time != SGS_TIME_INF) n->time -= zero_len;
		n->silence -= zero_len;
		if (!len) return zero_len;
		sbuf += zero_len;
	}
	/*
	 * Limit length to time duration of operator.
	 */
	skip_len = 0;
	if (n->time < (int)len && n->time != SGS_TIME_INF) {
		skip_len = len - n->time;
		len = n->time;
	}
	/*
	 * Handle frequency (alternatively ratio) parameter, including frequency
	 * modulation if modulators linked.
	 */
	freq = *(nextbuf++);
	if (n->attr & SGS_ATTR_VALITFREQ) {
		vi = &n->valitfreq;
		if (n->attr & SGS_ATTR_VALITFREQRATIO) {
			freqmod = parent_freq;
			if (!(n->attr & SGS_ATTR_FREQRATIO)) {
				n->attr |= SGS_ATTR_FREQRATIO;
				n->freq /= parent_freq[0].f;
			}
		} else {
			freqmod = 0;
			if (n->attr & SGS_ATTR_FREQRATIO) {
				n->attr &= ~SGS_ATTR_FREQRATIO;
				n->freq *= parent_freq[0].f;
			}
		}
	} else {
		vi = 0;
		freqmod = (n->attr & SGS_ATTR_FREQRATIO) ? parent_freq : 0;
	}
	if (run_param(freq, len, vi, &n->freq, freqmod))
		n->attr &= ~(SGS_ATTR_VALITFREQ|SGS_ATTR_VALITFREQRATIO);
	if (fmodc) {
		const int *fmods = n->adjcs->adjcs;
		BufData *fmbuf;
		for (i = 0; i < fmodc; ++i)
			run_block(o, nextbuf, len, &o->operators[fmods[i]], freq, 1, i);
		fmbuf = *nextbuf;
		if (n->attr & SGS_ATTR_FREQRATIO) {
			for (i = 0; i < len; ++i)
				freq[i].f += (n->dynfreq * parent_freq[i].f - freq[i].f) * fmbuf[i].f;
		} else {
			for (i = 0; i < len; ++i)
				freq[i].f += (n->dynfreq - freq[i].f) * fmbuf[i].f;
		}
	}
	/*
	 * If phase modulators linked, get phase offsets for modulation.
	 */
	pm = 0;
	if (pmodc) {
		const int *pmods = &n->adjcs->adjcs[fmodc];
		for (i = 0; i < pmodc; ++i)
			run_block(o, nextbuf, len, &o->operators[pmods[i]], freq, 0, i);
		pm = *(nextbuf++);
	}
	if (!wave_env) {
		/*
		 * Handle amplitude parameter, including amplitude modulation if
		 * modulators linked.
		 */
		if (amodc) {
			const int *amods = &n->adjcs->adjcs[fmodc+pmodc];
			float dynampdiff = n->dynamp - n->amp;
			for (i = 0; i < amodc; ++i)
				run_block(o, nextbuf, len, &o->operators[amods[i]], freq, 1, i);
			amp = *(nextbuf++);
			for (i = 0; i < len; ++i)
				amp[i].f = n->amp + amp[i].f * dynampdiff;
		} else {
			amp = *(nextbuf++);
			vi = (n->attr & SGS_ATTR_VALITAMP) ? &n->valitamp : 0;
			if (run_param(amp, len, vi, &n->amp, 0))
				n->attr &= ~SGS_ATTR_VALITAMP;
		}
		/*
		 * Generate integer output - either for voice output or phase modulation
		 * input.
		 */
		for (i = 0; i < len; ++i) {
			int s, spm = 0;
			float sfreq = freq[i].f, samp = amp[i].f;
			if (pm) spm = pm[i].i;
			SGSOsc_RUN_PM(&n->osc, n->osctype, o->osc_coeff, sfreq, spm, samp, s);
			if (acc_ind) s += sbuf[i].i;
			sbuf[i].i = s;
		}
	} else {
		/*
		 * Generate float output - used as waveform envelopes for modulating
		 * frequency or amplitude.
		 */
		for (i = 0; i < len; ++i) {
			float s, sfreq = freq[i].f;
			int spm = 0;
			if (pm) spm = pm[i].i;
			SGSOsc_RUN_PM_ENVO(&n->osc, n->osctype, o->osc_coeff, sfreq, spm, s);
			if (acc_ind) s *= sbuf[i].f;
			sbuf[i].f = s;
		}
	}
	/*
	 * Update time duration left, zero rest of buffer if unfilled.
	 */
	if (n->time != SGS_TIME_INF) {
		if (!acc_ind && skip_len > 0) {
			sbuf += len;
			for (i = 0; i < skip_len; ++i)
				sbuf[i].i = 0;
		}
		n->time -= len;
	}
	return zero_len + len;
}

/*
 * Generate up to buf_len samples for a voice, these mixed into the
 * interleaved output stereo buffer by simple addition.
 *
 * Returns number of samples generated for the voice.
 */
static uint run_voice(SGSGenerator *o, VoiceNode *vn, short *out,
											uint buf_len) {
	const int *ops;
	uint i, opc, ret_len = 0, finished = 1;
	int time;
	short *sp;
	if (!vn->graph) goto RETURN;
	opc = vn->graph->opc;
	ops = vn->graph->ops;
	time = 0;
	for (i = 0; i < opc; ++i) {
		OperatorNode *n = &o->operators[ops[i]];
		if (n->time == 0) continue;
		if (n->time > time && n->time != SGS_TIME_INF) time = n->time;
	}
	if (time > (int)buf_len) time = buf_len;
	/*
	 * Repeatedly generate up to BUF_LEN samples until done.
	 */
	sp = out;
	while (time) {
		uint acc_ind = 0;
		uint gen_len = 0;
		uint len = (time < BUF_LEN) ? time : BUF_LEN;
		time -= len;
		for (i = 0; i < opc; ++i) {
			uint last_len;
			OperatorNode *n = &o->operators[ops[i]];
			if (n->time == 0) continue;
			last_len = run_block(o, o->blocks, len, n, 0, 0, acc_ind++);
			if (last_len > gen_len) gen_len = last_len;
		}
		if (!gen_len) goto RETURN;
		if (vn->attr & SGS_ATTR_VALITPANNING) {
			BufData *buf = o->blocks[1];
			if (run_param(buf, gen_len, &vn->valitpanning, &vn->panning, 0))
				vn->attr &= ~SGS_ATTR_VALITPANNING;
			for (i = 0; i < gen_len; ++i) {
				int s = (*o->blocks)[i].i, p;
				SET_I2FV(p, ((float)s) * buf[i].f);
				*sp++ += s - p;
				*sp++ += p;
			}
		} else {
			for (i = 0; i < gen_len; ++i) {
				int s = (*o->blocks)[i].i, p;
				SET_I2FV(p, ((float)s) * vn->panning);
				*sp++ += s - p;
				*sp++ += p;
			}
		}
		ret_len += gen_len;
	}
	for (i = 0; i < opc; ++i) {
		OperatorNode *n = &o->operators[ops[i]];
		if (n->time != 0) {
			finished = 0;
			break;
		}
	}
RETURN:
	vn->pos += ret_len;
	if (finished)
		vn->flag &= ~SGS_FLAG_EXEC;
	return ret_len;
}

#endif

#if 0 // PROGRAM

#include "sgensys.h"
#include "program.h"
#include "parser.h"
#include <string.h>
#include <stdlib.h>

static void print_linked(const char *header, const char *footer, uint count,
		const int *nodes) {
	uint i;
	if (!count) return;
	printf("%s%d", header, nodes[0]);
	for (i = 0; ++i < count; )
		printf(", %d", nodes[i]);
	printf("%s", footer);
}

#if 0 /* old program.c code */

static void build_graph(SGSProgramEvent *root, const SGSEventNode *voice_in) {
	SGSOperatorNode **nl;
	SGSImGraph *graph, **graph_out;
	uint i;
	uint size;
	if (!voice_in->voice_params & SGS_GRAPH)
		return;
	size = voice_in->graph.count;
	graph_out = (SGSImGraph**)&root->voice->graph;
	if (!size) {
		*graph_out = graph;
		return;
	}
	nl = SGS_NODE_LIST_GET(&voice_in->graph);
	graph = malloc(sizeof(SGSImGraph) + sizeof(int) * (size - 1));
	graph->opc = size;
	for (i = 0; i < size; ++i)
		graph->ops[i] = nl[i]->operator_id;
	*graph_out = graph;
}

static void build_adjcs(SGSProgramEvent *root,
		const SGSOperatorNode *operator_in) {
	SGSOperatorNode **nl;
	SGSImGraphNodeAdjcs *adjcs, **adjcs_out;
	int *data;
	uint i;
	uint size;
	if (!operator_in || !(operator_in->operator_params & SGS_ADJCS))
		return;
	size = operator_in->fmods.count +
				 operator_in->pmods.count +
				 operator_in->amods.count;
	adjcs_out = (SGSImGraphNodeAdjcs**)&root->operator->adjcs;
	if (!size) {
		*adjcs_out = 0;
		return;
	}
	adjcs = malloc(sizeof(SGSImGraphNodeAdjcs) + sizeof(int) * (size - 1));
	adjcs->fmodc = operator_in->fmods.count;
	adjcs->pmodc = operator_in->pmods.count;
	adjcs->amodc = operator_in->amods.count;
	data = adjcs->adjcs;
	nl = SGS_NODE_LIST_GET(&operator_in->fmods);
	for (i = 0; i < adjcs->fmodc; ++i)
		*data++ = nl[i]->operator_id;
	nl = SGS_NODE_LIST_GET(&operator_in->pmods);
	for (i = 0; i < adjcs->pmodc; ++i)
		*data++ = nl[i]->operator_id;
	nl = SGS_NODE_LIST_GET(&operator_in->amods);
	for (i = 0; i < adjcs->amodc; ++i)
		*data++ = nl[i]->operator_id;
	*adjcs_out = adjcs;
}

#endif



/*
static void build_voicedata(SGSProgramEvent *root,
		const SGSEventNode *voice_in) {
	SGSOperatorNode **nl;
	SGSProgramVoiceData *graph, **graph_out;
	uint i;
	uint size;
	if (!voice_in->voice_params & SGS_GRAPH)
		return;
	size = voice_in->graph.count;
	graph_out = (SGSImGraph**)&root->voice->graph;
	if (!size) {
		*graph_out = graph;
		return;
	}
	nl = SGS_NODE_LIST_GET(&voice_in->graph);
	graph = malloc(sizeof(SGSImGraph) + sizeof(int) * (size - 1));
	graph->opc = size;
	for (i = 0; i < size; ++i)
		graph->ops[i] = nl[i]->operator_id;
	*graph_out = graph;
}
*/


/*
 * Program (event, voice, operator) allocation
 */

typedef struct VoiceAllocData {
	SGSEventNode *last;
	uint duration_ms;
} VoiceAllocData;

typedef struct VoiceAlloc {
	VoiceAllocData *data;
	uint voicec;
	uint alloc;
} VoiceAlloc;

#define VOICE_ALLOC_INIT(va) do{ \
	(va)->data = calloc(1, sizeof(VoiceAllocData)); \
	(va)->voicec = 0; \
	(va)->alloc = 1; \
}while(0)

#define VOICE_ALLOC_FINI(va, prg) do{ \
	(prg)->voicec = (va)->voicec; \
	free((va)->data); \
}while(0)

/*
 * Returns the longest operator duration among top-level operators for
 * the graph of the voice event.
 */
static uint voice_duration(SGSEventNode *ve) {
	SGSOperatorNode **nl = SGS_NODE_LIST_GET(&ve->operators);
	uint i, duration_ms = 0;
	/* FIXME: node list type? */
	for (i = 0; i < ve->operators.count; ++i) {
		SGSOperatorNode *op = nl[i];
		if (op->time_ms > (int)duration_ms)
			duration_ms = op->time_ms;
	}
	return duration_ms;
}

/*
 * Incremental voice allocation - allocate voice for event,
 * returning voice id.
 */
static uint voice_alloc_inc(VoiceAlloc *va, SGSEventNode *e) {
	uint voice;
	for (voice = 0; voice < va->voicec; ++voice) {
		if ((int)va->data[voice].duration_ms < e->wait_ms)
			va->data[voice].duration_ms = 0;
		else
			va->data[voice].duration_ms -= e->wait_ms;
	}
	if (e->voice_prev) {
		SGSEventNode *prev = e->voice_prev;
		voice = prev->voice_id;
	} else {
		for (voice = 0; voice < va->voicec; ++voice)
			if (!(va->data[voice].last->en_flags & EN_VOICE_LATER_USED) &&
					va->data[voice].duration_ms == 0) break;
		/*
		 * If no unused voice found, allocate new one.
		 */
		if (voice == va->voicec) {
			++va->voicec;
			if (va->voicec > va->alloc) {
				uint i = va->alloc;
				va->alloc <<= 1;
				va->data = realloc(va->data, va->alloc * sizeof(VoiceAllocData));
				while (i < va->alloc) {
					va->data[i].last = 0;
					va->data[i].duration_ms = 0;
					++i;
				}
			}
		}
	}
	e->voice_id = voice;
	va->data[voice].last = e;
//	if (e->voice_params & SGS_GRAPH)
		va->data[voice].duration_ms = voice_duration(e);
	return voice;
}

typedef struct OperatorAllocData {
	SGSOperatorNode *last;
	SGSProgramEvent *out;
	uint duration_ms;
	uchar visited; /* used in graph traversal */
} OperatorAllocData;

typedef struct OperatorAlloc {
	OperatorAllocData *data;
	uint operatorc;
	uint alloc;
} OperatorAlloc;

#define OPERATOR_ALLOC_INIT(oa) do{ \
	(oa)->data = calloc(1, sizeof(OperatorAllocData)); \
	(oa)->operatorc = 0; \
	(oa)->alloc = 1; \
}while(0)

#define OPERATOR_ALLOC_FINI(oa, prg) do{ \
	(prg)->operatorc = (oa)->operatorc; \
	free((oa)->data); \
}while(0)

/*
 * Incremental operator allocation - allocate operator for event,
 * returning operator id.
 *
 * Only valid to call for single-operator nodes.
 */
static uint operator_alloc_inc(OperatorAlloc *oa, SGSOperatorNode *op) {
	SGSEventNode *e = op->event;
	uint operator;
	for (operator = 0; operator < oa->operatorc; ++operator) {
		if ((int)oa->data[operator].duration_ms < e->wait_ms)
			oa->data[operator].duration_ms = 0;
		else
			oa->data[operator].duration_ms -= e->wait_ms;
	}
	if (op->on_prev) {
		SGSOperatorNode *pop = op->on_prev;
		operator = pop->operator_id;
	} else {
//		for (operator = 0; operator < oa->operatorc; ++operator)
//			if (!(oa->data[operator].last->on_flags & ON_OPERATOR_LATER_USED) &&
//					oa->data[operator].duration_ms == 0) break;
		/*
		 * If no unused operator found, allocate new one.
		 */
		if (operator == oa->operatorc) {
			++oa->operatorc;
			if (oa->operatorc > oa->alloc) {
				uint i = oa->alloc;
				oa->alloc <<= 1;
				oa->data = realloc(oa->data, oa->alloc * sizeof(OperatorAllocData));
				while (i < oa->alloc) {
					oa->data[i].last = 0;
					oa->data[i].duration_ms = 0;
					++i;
				}
			}
		}
	}
	op->operator_id = operator;
	oa->data[operator].last = op;
//	oa->data[operator].duration_ms = op->time_ms;
	return operator;
}

typedef struct ProgramAlloc {
	SGSProgramEvent *oe, **oevents;
	uint eventc;
	uint alloc;
	OperatorAlloc oa;
	VoiceAlloc va;
} ProgramAlloc;

#define PROGRAM_ALLOC_INIT(pa) do{ \
	VOICE_ALLOC_INIT(&(pa)->va); \
	OPERATOR_ALLOC_INIT(&(pa)->oa); \
	(pa)->oe = 0; \
	(pa)->oevents = 0; \
	(pa)->eventc = 0; \
	(pa)->alloc = 0; \
}while(0)

#define PROGRAM_ALLOC_FINI(pa, prg) do{ \
	uint i; \
	/* copy output events to program & cleanup */ \
	*(SGSProgramEvent**)&(prg)->events = malloc(sizeof(SGSProgramEvent) * \
		(pa)->eventc); \
	for (i = 0; i < (pa)->eventc; ++i) { \
		*(SGSProgramEvent*)&(prg)->events[i] = *(pa)->oevents[i]; \
		free((pa)->oevents[i]); \
	} \
	free((pa)->oevents); \
	(prg)->eventc = (pa)->eventc; \
	OPERATOR_ALLOC_FINI(&(pa)->oa, (prg)); \
	VOICE_ALLOC_FINI(&(pa)->va, (prg)); \
}while(0)

static SGSProgramEvent *program_alloc_oevent(ProgramAlloc *pa) {
	++pa->eventc;
	if (pa->eventc > pa->alloc) {
		pa->alloc = (pa->alloc > 0) ? pa->alloc << 1 : 1;
		pa->oevents = realloc(pa->oevents, sizeof(SGSProgramEvent*) * pa->alloc);
	}
	pa->oevents[pa->eventc - 1] = calloc(1, sizeof(SGSProgramEvent));
	pa->oe = pa->oevents[pa->eventc - 1];
	return pa->oe;
}

/*
 * Graph creation and traversal
 */

#if 0
typedef struct SGSImGraph {
  uchar opc;
  struct SGSImGraphNodeAdjcs *ops[1]; /* sized to opc */
} SGSImGraph;

typedef struct SGSImGraphNodeAdjcs {
  uchar pmodc;
  uchar fmodc;
  uchar amodc;
  uchar level;  /* index for buffer used to store result to use if node
                   revisited when traversing the graph. */
  struct SGSImGraphNodeAdjcs *adjcs[1]; /* sized to total number */
} SGSImGraphNodeAdjcs;

static SGSImGraphNodeAdjcs *create_adjcs(ProgramAlloc *pa,
		const SGSOperatorNode *operator_in) {
	SGSOperatorNode **nl;
	SGSImGraphNodeAdjcs *adjcs;
	SGSImGraphNodeAdjcs **data;
	uint i;
	uint size;
	if (!operator_in || !(operator_in->event->voice_params & SGS_OPLIST))
		return NULL;
	size = operator_in->fmods.count +
		operator_in->pmods.count +
		operator_in->amods.count;
	if (size == 0) {
		return NULL;
	}
	adjcs = malloc(sizeof(SGSImGraphNodeAdjcs) +
		sizeof(SGSImGraphNodeAdjcs*) * (size - 1));
	adjcs->fmodc = operator_in->fmods.count;
	adjcs->pmodc = operator_in->pmods.count;
	adjcs->amodc = operator_in->amods.count;
	data = adjcs->adjcs;
	nl = SGS_NODE_LIST_GET(&operator_in->fmods);
	for (i = 0; i < adjcs->fmodc; ++i) {
		int op_id = nl[i]->operator_id;
		OperatorAllocData *op_data = &pa->oa.data[op_id];
		SGSOperatorNode *last = op_data->last;
		*data++ = create_adjcs(pa, last);
	}
	nl = SGS_NODE_LIST_GET(&operator_in->pmods);
	for (i = 0; i < adjcs->pmodc; ++i) {
		int op_id = nl[i]->operator_id;
		OperatorAllocData *op_data = &pa->oa.data[op_id];
		SGSOperatorNode *last = op_data->last;
		*data++ = create_adjcs(pa, last);
	}
	nl = SGS_NODE_LIST_GET(&operator_in->amods);
	for (i = 0; i < adjcs->amodc; ++i) {
		int op_id = nl[i]->operator_id;
		OperatorAllocData *op_data = &pa->oa.data[op_id];
		SGSOperatorNode *last = op_data->last;
		*data++ = create_adjcs(pa, last);
	}
	return adjcs;
}

static void destroy_adjcs(SGSImGraphNodeAdjcs *o) {
	uint i, total;
	if (o == NULL) return;
	total = o->fmodc +
		o->pmodc +
		o->amodc;
	for (i = 0; i < total; ++i) {
		destroy_adjcs(o->adjcs[i]);
	}
	free(o);
}

static SGSImGraph *create_graph(ProgramAlloc *pa,
		const SGSEventNode *voice_in) {
	SGSOperatorNode **nl;
	SGSImGraph *graph;
	uint i;
	uint size;
	if (!voice_in->voice_params & SGS_OPLIST)
		return NULL;
	size = voice_in->graph.count;
	if (!size) {
		return NULL;
	}
	graph = malloc(sizeof(SGSImGraph) +
		sizeof(SGSImGraphNodeAdjcs*) * (size - 1));
	graph->opc = size;
	nl = SGS_NODE_LIST_GET(&voice_in->graph);
	for (i = 0; i < size; ++i) {
		int op_id = nl[i]->operator_id;
		OperatorAllocData *op_data = &pa->oa.data[op_id];
		SGSOperatorNode *last = op_data->last;
		graph->ops[i] = create_adjcs(pa, last);
	}
	return graph;
}

static void destroy_graph(SGSImGraph *o) {
	uint i;
	for (i = 0; i < o->opc; ++i) {
		destroy_adjcs(o->ops[i]);
	}
	free(o);
}
#endif

#if 0 /* generator.c */
static uint run_block(SGSGenerator *o, Buf *blocks, uint buf_len,
		OperatorNode *n, BufData *parent_freq, uchar wave_env,
		uint acc_ind) {
	uint i, len, zero_len, skip_len;
	BufData *freq, *freqmod, *pm, *amp;
	BufData *sbuf = *blocks;
	Buf *nextbuf = blocks + 1;
	uchar fmodc, pmodc, amodc;
	/*
	 * Handle frequency (alternatively ratio) parameter, including frequency
	 * modulation if modulators linked.
	 */
	freq = *(nextbuf++);
	if (n->attr & SGS_ATTR_VALITFREQ) {
		if (n->attr & SGS_ATTR_VALITFREQRATIO) {
			freqmod = parent_freq;
			if (!(n->attr & SGS_ATTR_FREQRATIO)) {
				n->attr |= SGS_ATTR_FREQRATIO;
				n->freq /= parent_freq[0].f;
			}
		} else {
			freqmod = 0;
			if (n->attr & SGS_ATTR_FREQRATIO) {
				n->attr &= ~SGS_ATTR_FREQRATIO;
				n->freq *= parent_freq[0].f;
			}
		}
	} else {
		freqmod = (n->attr & SGS_ATTR_FREQRATIO) ? parent_freq : 0;
	}
	if (run_param(freq, len, vi, &n->freq, freqmod))
		n->attr &= ~(SGS_ATTR_VALITFREQ|SGS_ATTR_VALITFREQRATIO);
	if (fmodc) {
		const int *fmods = n->adjcs->adjcs;
		BufData *fmbuf;
		for (i = 0; i < fmodc; ++i)
			run_block(o, nextbuf, len, &o->operators[fmods[i]], freq, 1, i);
		fmbuf = *nextbuf;
		if (n->attr & SGS_ATTR_FREQRATIO) {
			for (i = 0; i < len; ++i)
				freq[i].f += (n->dynfreq * parent_freq[i].f - freq[i].f) * fmbuf[i].f;
		} else {
			for (i = 0; i < len; ++i)
				freq[i].f += (n->dynfreq - freq[i].f) * fmbuf[i].f;
		}
	}
	/*
	 * If phase modulators linked, get phase offsets for modulation.
	 */
	pm = 0;
	if (pmodc) {
		const int *pmods = &n->adjcs->adjcs[fmodc];
		for (i = 0; i < pmodc; ++i)
			run_block(o, nextbuf, len, &o->operators[pmods[i]], freq, 0, i);
		pm = *(nextbuf++);
	}
	if (!wave_env) {
		/*
		 * Handle amplitude parameter, including amplitude modulation if
		 * modulators linked.
		 */
		if (amodc) {
			const int *amods = &n->adjcs->adjcs[fmodc+pmodc];
			float dynampdiff = n->dynamp - n->amp;
			for (i = 0; i < amodc; ++i)
				run_block(o, nextbuf, len, &o->operators[amods[i]], freq, 1, i);
			amp = *(nextbuf++);
			for (i = 0; i < len; ++i)
				amp[i].f = n->amp + amp[i].f * dynampdiff;
		} else {
			amp = *(nextbuf++);
			vi = (n->attr & SGS_ATTR_VALITAMP) ? &n->valitamp : 0;
			if (run_param(amp, len, vi, &n->amp, 0))
				n->attr &= ~SGS_ATTR_VALITAMP;
		}
		/*
		 * Generate integer output - either for voice output or phase modulation
		 * input.
		 */
		for (i = 0; i < len; ++i) {
			int s, spm = 0;
			float sfreq = freq[i].f, samp = amp[i].f;
			if (pm) spm = pm[i].i;
			SGSOsc_RUN_PM(&n->osc, n->osctype, o->osc_coeff, sfreq, spm, samp, s);
			if (acc_ind) s += sbuf[i].i;
			sbuf[i].i = s;
		}
	} else {
		/*
		 * Generate float output - used as waveform envelopes for modulating
		 * frequency or amplitude.
		 */
		for (i = 0; i < len; ++i) {
			float s, sfreq = freq[i].f;
			int spm = 0;
			if (pm) spm = pm[i].i;
			SGSOsc_RUN_PM_ENVO(&n->osc, n->osctype, o->osc_coeff, sfreq, spm, s);
			if (acc_ind) s *= sbuf[i].f;
			sbuf[i].f = s;
		}
	}
}
#endif

static const int *traverse_graph(SGSImGraph *operator_graph) {
	const int *op_list;
	
	return op_list;
}

static void reset_graph(ProgramAlloc *pa) {
	OperatorAlloc *oa = &pa->oa;
	uint i;
	for (i = 0; i < oa->operatorc; ++i) {
		OperatorAllocData *entry = &oa->data[i];
		entry->visited = 0;
	}
}

/*
 * Overwrite parameters in dst that have values in src.
 */
static void copy_params(SGSOperatorNode *dst, const SGSOperatorNode *src) {
	if (src->operator_params & SGS_AMP) dst->amp = src->amp;
}

static void expand_operator(SGSOperatorNode *op) {
	SGSOperatorNode *pop;
	if (!(op->on_flags & ON_MULTIPLE_OPERATORS)) return;
	pop = op->on_prev;
	do {
		copy_params(pop, op);
		expand_operator(pop);
	} while ((pop = pop->next_bound));
	SGS_node_list_clear(&op->fmods);
	SGS_node_list_clear(&op->pmods);
	SGS_node_list_clear(&op->amods);
	op->operator_params = 0;
}

/*
 * Convert data for an operator node to program operator data, setting it for
 * the program event given.
 */
static void program_convert_onode(ProgramAlloc *pa, SGSOperatorNode *op,
		uint operator_id) {
	SGSProgramEvent *oe = pa->oa.data[operator_id].out;
	SGSProgramOperatorData *ood = calloc(1, sizeof(SGSProgramOperatorData));
	oe->operator = ood;
	oe->params |= op->operator_params;
	//printf("operator_id == %d | address == %x\n", op->operator_id, op);
	ood->operator_id = operator_id;
//	ood->output_block_id = 
	ood->freq_block_id = -1;
	ood->freq_mod_block_id = -1;
	ood->phase_mod_block_id = -1;
	ood->amp_block_id = -1;
	ood->amp_mod_block_id = -1;
	ood->attr = op->attr;
	ood->wave = op->wave;
	ood->time_ms = op->time_ms;
	ood->silence_ms = op->silence_ms;
	ood->freq = op->freq;
	ood->dynfreq = op->dynfreq;
	ood->phase = op->phase;
	ood->amp = op->amp;
	ood->dynamp = op->dynamp;
	ood->valitfreq = op->valitfreq;
	ood->valitamp = op->valitamp;
}

/*
 * Visit each operator node in the node list and recurse through each node's
 * sublists in turn, following and converting operator data and allocating
 * new output events as needed.
 */
static void program_follow_onodes(ProgramAlloc *pa, SGSNodeList *nl) {
	uint i;
	SGSOperatorNode **list = SGS_NODE_LIST_GET(nl);
	for (i = nl->inactive_count; i < nl->count; ++i) {
		SGSOperatorNode *op = list[i];
		OperatorAllocData *ad;
		uint operator_id;
		if (op->on_flags & ON_MULTIPLE_OPERATORS) continue;
		operator_id = operator_alloc_inc(&pa->oa, op);
		program_follow_onodes(pa, &op->fmods);
		program_follow_onodes(pa, &op->pmods);
		program_follow_onodes(pa, &op->amods);
		ad = &pa->oa.data[operator_id];
		if (pa->oe->operator) {
			/*
			 * Need a new output event for this operator;
			 * the current one is already used.
			 */
			program_alloc_oevent(pa);
		}
		ad->out = pa->oe;
		program_convert_onode(pa, op, operator_id);
	}
}

/*
 * Convert voice and operator data for an event node into a (series of) output
 * events.
 *
 * This is the "main" parser data conversion function, to be called for every
 * event.
 */
static void program_convert_enode(ProgramAlloc *pa, SGSEventNode *e) {
	SGSProgramEvent *oe;
	SGSProgramVoiceData *ovd;
	uint voice_id;
	/* Add to final output list */
	voice_id = voice_alloc_inc(&pa->va, e);
	oe = program_alloc_oevent(pa);
	oe->wait_ms = e->wait_ms;
	if (e->voice_params & SGS_OPLIST) {
	}
	program_follow_onodes(pa, &e->operators);
	oe = pa->oe; /* oe may have re(al)located */
	if (e->voice_params & SGS_OPLIST) {
		SGSImGraph *graph;
		ovd = calloc(1, sizeof(SGSProgramVoiceData));
		oe->voice = ovd;
		oe->params |= e->voice_params;
		ovd->voice_id = voice_id;
		ovd->attr = e->voice_attr;
		ovd->panning = e->panning;
		ovd->valitpanning = e->valitpanning;
		graph = create_graph(pa, e);
		destroy_graph(graph);
	}
}

static SGSProgram* build(SGSParser *o) {
	//puts("build():");
	ProgramAlloc pa;
	SGSProgram *prg = calloc(1, sizeof(SGSProgram));
	SGSEventNode *e;
	uint id;
	/*
	 * Pass #1 - Output event allocation, voice allocation,
	 *					 parameter data copying.
	 */
	PROGRAM_ALLOC_INIT(&pa);
	for (e = o->events; e; e = e->next) {
		program_convert_enode(&pa, e);
	}
	PROGRAM_ALLOC_FINI(&pa, prg);
	/*
	 * Pass #2 - Cleanup of parsing data.
	 */
	for (e = o->events; e; ) {
		SGSEventNode *e_next = e->next;
		SGS_event_node_destroy(e);
		e = e_next;
	}
	//puts("/build()");
#if 1
	/*
	 * Debug printing.
	 */
	putchar('\n');
	printf("events: %d\tvoices: %d\toperators: %d\n", prg->eventc, prg->voicec, prg->operatorc);
	for (id = 0; id < prg->eventc; ++id) {
		const SGSProgramEvent *oe;
		const SGSProgramVoiceData *ovo;
		const SGSProgramOperatorData *oop;
		oe = &prg->events[id];
		ovo = oe->voice;
		oop = oe->operator;
		printf("\\%d \tEV %d", oe->wait_ms, id);
		if (ovo) {
//			const SGSImGraph *g = ovo->graph;
			printf("\n\tvo %d", ovo->voice_id);
//			if (g)
//				print_linked("\n\t		{", "}", g->opc, g->ops);
		}
		if (oop) {
//			const SGSImGraphNodeAdjcs *ga = oop->adjcs;
			if (oop->time_ms == SGS_TIME_INF)
				printf("\n\top %d \tt=INF \tf=%.f", oop->operator_id, oop->freq);
			else
				printf("\n\top %d \tt=%d \tf=%.f", oop->operator_id, oop->time_ms, oop->freq);
//			if (ga) {
//				print_linked("\n\t		f!<", ">", ga->fmodc, ga->adjcs);
//				print_linked("\n\t		p!<", ">", ga->pmodc, &ga->adjcs[ga->fmodc]);
//				print_linked("\n\t		a!<", ">", ga->amodc, &ga->adjcs[ga->fmodc +
//					ga->pmodc]);
//			}
		}
		putchar('\n');
	}
#endif
	return prg;
}

SGSProgram* SGS_program_create(const char *filename) {
	SGSParser p;
	FILE *f = fopen(filename, "r");
	if (!f) return 0;

	SGS_parse(&p, f, filename);
	fclose(f);
	return build(&p);
}

void SGS_program_destroy(SGSProgram *o) {
	uint i;
	for (i = 0; i < o->eventc; ++i) {
		SGSProgramEvent *e = (void*)&o->events[i];
		if (e->voice) {
			free((void*)e->voice->operator_list);
			free((void*)e->voice);
		}
		if (e->operator) {
			free((void*)e->operator);
		}
	}
	free((void*)o->events);
}

#endif
