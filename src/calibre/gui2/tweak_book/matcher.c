/*
 * matcher.c
 * Copyright (C) 2014 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#define NO_ICU_TO_PYTHON
#include "icu_calibre_utils.h"

#ifdef _MSC_VER
// inline does not work with the visual studio C compiler
#define inline
#endif

typedef unsigned char bool;
#define TRUE 1
#define FALSE 0
#define MIN(x, y) ((x < y) ? x : y)
#define MAX(x, y) ((x > y) ? x : y)
#define nullfree(x) if(x != NULL) free(x); x = NULL;

// Algorithm to sort items by subsequence score {{{
typedef struct {
    double score;
    int32_t *positions;
} MemoryItem;

static MemoryItem*** alloc_memory(int32_t needle_len, int32_t max_haystack_len) {
    MemoryItem ***ans = NULL, **d1 = NULL, *d2 = NULL;
    size_t num = max_haystack_len * max_haystack_len * needle_len;
    size_t position_sz = needle_len * sizeof(int32_t);
    size_t sz = (num * (sizeof(MemoryItem) + position_sz)) + (max_haystack_len * sizeof(MemoryItem**)) + (needle_len * sizeof(MemoryItem*));
    int32_t hidx, nidx, last_idx, i, j;
    char *base = NULL;

    ans = (MemoryItem***) calloc(sz, 1);
    if (ans != NULL) {
        d1 = (MemoryItem**)(ans + max_haystack_len);
        d2 = (MemoryItem*) (d1 + max_haystack_len * needle_len );
        for (i = 0; i < max_haystack_len; i++) {
            ans[i] = d1 + i * needle_len;
            for (j = 0; j < needle_len; j++) d1[i*needle_len + j] = d2 + j;
        }

        base = ((char*)ans) + (sizeof(MemoryItem**)*max_haystack_len) + (sizeof(MemoryItem*)*needle_len) + (sizeof(MemoryItem)*max_haystack_len);

        for (hidx = 0; hidx < max_haystack_len; hidx++) {
            for (nidx = 0; nidx < needle_len; nidx++) {
                for (last_idx = 0; last_idx < max_haystack_len; last_idx++) {
                    ans[hidx][nidx][last_idx].positions = (int32_t*)base;
                    base += position_sz;
                }
            }
        }
    }
    return ans;
}

static void clear_memory(MemoryItem ***mem, int32_t needle_len, int32_t max_haystack_len) {
    int32_t hidx, nidx, last_idx;
    for (hidx = 0; hidx < max_haystack_len; hidx++) {
        for (nidx = 0; nidx < needle_len; nidx++) {
            for (last_idx = 0; last_idx < max_haystack_len; last_idx++) {
                mem[hidx][nidx][last_idx].score = DBL_MAX;
            }
        }
    }
}

typedef struct {
    int32_t hidx;
    int32_t nidx;
    int32_t last_idx;
    double score;
    int32_t *positions;
} StackItem;

typedef struct {
    ssize_t pos;
    int32_t needle_len;
    size_t size;
    StackItem *items;
} Stack;

static void alloc_stack(Stack *stack, int32_t needle_len, int32_t max_haystack_len) {
    StackItem *ans = NULL;
    char *base = NULL;
    size_t num = max_haystack_len * needle_len;
    size_t position_sz = needle_len * sizeof(int32_t);
    size_t sz = sizeof(StackItem) + position_sz;
    size_t i = 0;

    stack->needle_len = needle_len;
    stack->pos = -1;
    stack->size = num;
    ans = (StackItem*) calloc(num, sz);
    if (ans != NULL) {
        base = (char*)(ans + num);
        for (i = 0; i < num; i++, base += position_sz) ans[i].positions = (int32_t*) base;
        stack->items = ans;
    }
}

static void stack_clear(Stack *stack) { stack->pos = -1; }

static void stack_push(Stack *stack, int32_t hidx, int32_t nidx, int32_t last_idx, double score, int32_t *positions) {
    StackItem *si = &(stack->items[++stack->pos]);
    si->hidx = hidx; si->nidx = nidx; si->last_idx = last_idx; si->score = score;
    memcpy(si->positions, positions, sizeof(*positions) * stack->needle_len);
}

static void stack_pop(Stack *stack, int32_t *hidx, int32_t *nidx, int32_t *last_idx, double *score, int32_t *positions) {
    StackItem *si = &(stack->items[stack->pos--]);
    *hidx = si->hidx; *nidx = si->nidx; *last_idx = si->last_idx; *score = si->score;
    memcpy(positions, si->positions, sizeof(*positions) * stack->needle_len);
}

typedef struct {
    UChar *haystack;
    int32_t haystack_len;
    UChar *needle;
    int32_t needle_len;
    double max_score_per_char;
    MemoryItem ***memo;
    UChar *level1;
    UChar *level2;
    UChar *level3;
} MatchInfo;

typedef struct {
    double score;
    int32_t *positions;
} Match;


static double calc_score_for_char(MatchInfo *m, UChar32 last, UChar32 current, int32_t distance_from_last_match) {
    double factor = 1.0;
    double ans = m->max_score_per_char;

    if (u_strchr32(m->level1, last) != NULL)
        factor = 0.9;
    else if (u_strchr32(m->level2, last) != NULL)
        factor = 0.8;
    else if (u_isULowercase(last) && u_isUUppercase(current))
        factor = 0.8;  // CamelCase
    else if (u_strchr32(m->level3, last) != NULL)
        factor = 0.7;
    else
        // If last is not a special char, factor diminishes
        // as distance from last matched char increases
        factor = (1.0 / distance_from_last_match) * 0.75;
    return ans * factor;
}

static void convert_positions(int32_t *positions, int32_t *final_positions, UChar *string, int32_t char_len, int32_t byte_len, double score) {
    // The positions array stores character positions as byte offsets in string, convert them into character offsets
    int32_t i, *end;

    if (score == 0.0) {
        for (i = 0; i < char_len; i++) final_positions[i] = -1;
        return;
    }

    end = final_positions + char_len;
    for (i = 0; i < byte_len && final_positions < end; i++) {
        if (positions[i] == -1) continue;
        *final_positions = u_countChar32(string, positions[i]);
        final_positions += 1;
    }
}

static double process_item(MatchInfo *m, Stack *stack, int32_t *final_positions) {
    UChar32 nc, hc, lc;
    UChar *p;
    double final_score = 0.0, score = 0.0, score_for_char = 0.0;
    int32_t pos, i, j, hidx, nidx, last_idx, distance, *positions = final_positions + m->needle_len;
    MemoryItem mem = {0};

    stack_push(stack, 0, 0, 0, 0.0, final_positions);

    while (stack->pos >= 0) {
        stack_pop(stack, &hidx, &nidx, &last_idx, &score, positions);
        mem = m->memo[hidx][nidx][last_idx];
        if (mem.score == DBL_MAX) {
            // No memoized result, calculate the score
            for (i = nidx; i < m->needle_len;) {
                nidx = i;
                U16_NEXT(m->needle, i, m->needle_len, nc); // i now points to next char in needle 
                if (m->haystack_len - hidx < m->needle_len - nidx) { score = 0.0; break; }
                p = u_strchr32(m->haystack + hidx, nc);  // TODO: Use primary collation for the find
                if (p == NULL) { score = 0.0; break; }
                pos = (int32_t)(p - m->haystack);
                distance = u_countChar32(m->haystack + last_idx, pos - last_idx);  
                if (distance <= 1) score_for_char = m->max_score_per_char;
                else {
                    U16_GET(m->haystack, 0, pos, m->haystack_len, hc); 
                    j = pos;
                    U16_PREV(m->haystack, 0, j, lc); // lc is the prev character
                    score_for_char = calc_score_for_char(m, lc, hc, distance);
                }
                j = pos;
                U16_NEXT(m->haystack, j, m->haystack_len, hc); 
                hidx = j;
                if (m->haystack_len - hidx >= m->needle_len - nidx) stack_push(stack, hidx, nidx, last_idx, score, positions);
                last_idx = pos; 
                positions[nidx] = pos; 
                score += score_for_char;
            } // for(i) iterate over needle
            mem.score = score; memcpy(mem.positions, positions, sizeof(*positions) * m->needle_len);

        } else {
            score = mem.score; memcpy(positions, mem.positions, sizeof(*positions) * m->needle_len);
        }
        // We have calculated the score for this hidx, nidx, last_idx combination, update final_score and final_positions, if needed
        if (score > final_score) {
            final_score = score;
            memcpy(final_positions, positions, sizeof(*positions) * m->needle_len);
        }
    }
    return final_score;
}


static bool match(UChar **items, int32_t *item_lengths, uint32_t item_count, UChar *needle, Match *match_results, int32_t *final_positions, int32_t needle_char_len, UChar *level1, UChar *level2, UChar *level3) {
    Stack stack = {0};
    int32_t i = 0, maxhl = 0; 
    int32_t r = 0, *positions = NULL;
    MatchInfo *matches = NULL;
    bool ok = FALSE;
    MemoryItem ***memo = NULL;
    int32_t needle_len = u_strlen(needle);

    if (needle_len <= 0 || item_count <= 0) {
        for (i = 0; i < (int32_t)item_count; i++) match_results[i].score = 0.0;
        ok = TRUE;
        goto end;
    }

    matches = (MatchInfo*)calloc(item_count, sizeof(MatchInfo));
    positions = (int32_t*)calloc(2*needle_len, sizeof(int32_t)); // One set of positions is the final answer and one set is working space
    if (matches == NULL || positions == NULL) {PyErr_NoMemory(); goto end;}

    for (i = 0; i < (int32_t)item_count; i++) {
        matches[i].haystack = items[i];
        matches[i].haystack_len = item_lengths[i];
        matches[i].needle = needle;
        matches[i].needle_len = needle_len;
        matches[i].max_score_per_char = (1.0 / matches[i].haystack_len + 1.0 / needle_len) / 2.0;
        matches[i].level1 = level1;
        matches[i].level2 = level2;
        matches[i].level3 = level3;
        maxhl = MAX(maxhl, matches[i].haystack_len);
    }

    if (maxhl <= 0) {
        for (i = 0; i < (int32_t)item_count; i++) match_results[i].score = 0.0;
        ok = TRUE;
        goto end;
    }

    alloc_stack(&stack, needle_len, maxhl);
    memo = alloc_memory(needle_len, maxhl);
    if (stack.items == NULL || memo == NULL) {PyErr_NoMemory(); goto end;}

    for (i = 0; i < (int32_t)item_count; i++) {
        for (r = 0; r < needle_len; r++) {
            positions[r] = -1;
        }
        stack_clear(&stack);
        clear_memory(memo, needle_len, matches[i].haystack_len);
        matches[i].memo = memo;
        match_results[i].score = process_item(&matches[i], &stack, positions);
        convert_positions(positions, final_positions + i, matches[i].haystack, needle_char_len, needle_len, match_results[i].score);
    }

    ok = TRUE;
end:
    nullfree(positions);
    nullfree(stack.items);
    nullfree(matches);
    nullfree(memo);
    return ok;
}

// }}}

// Matcher object definition {{{
typedef struct {
    PyObject_HEAD
    // Type-specific fields go here.
    UChar **items;
    uint32_t item_count;
    int32_t *item_lengths;
    UChar *level1;
    UChar *level2;
    UChar *level3;

} Matcher;

// Matcher.__init__() {{{

static void free_matcher(Matcher *self) {
    uint32_t i = 0;
    if (self->items != NULL) {
        for (i = 0; i < self->item_count; i++) { nullfree(self->items[i]); }
    }
    nullfree(self->items); nullfree(self->item_lengths); 
    nullfree(self->level1); nullfree(self->level2); nullfree(self->level3);
}
static void
Matcher_dealloc(Matcher* self)
{
    free_matcher(self);
    self->ob_type->tp_free((PyObject*)self);
}

#define alloc_uchar(x) (x * 3 + 1)
static int
Matcher_init(Matcher *self, PyObject *args, PyObject *kwds)
{
    PyObject *items = NULL, *p = NULL, *py_items = NULL, *level1 = NULL, *level2 = NULL, *level3 = NULL;
    int32_t i = 0;

    if (!PyArg_ParseTuple(args, "OOOO", &items, &level1, &level2, &level3)) return -1;
    py_items = PySequence_Fast(items,  "Must pass in two sequence objects");
    if (py_items == NULL) goto end;
    self->item_count = (uint32_t)PySequence_Size(items);

    self->items = (UChar**)calloc(self->item_count, sizeof(UChar*));
    self->item_lengths = (int32_t*)calloc(self->item_count, sizeof(uint32_t));
    self->level1 = python_to_icu(level1, NULL, 1);
    self->level2 = python_to_icu(level2, NULL, 1);
    self->level3 = python_to_icu(level3, NULL, 1);

    if (self->items == NULL || self->item_lengths == NULL ) { PyErr_NoMemory(); goto end; }
    if (self->level1 == NULL || self->level2 == NULL || self->level3 == NULL) goto end;

    for (i = 0; i < (int32_t)self->item_count; i++) {
        p = PySequence_Fast_GET_ITEM(py_items, i);
        self->items[i] = python_to_icu(p, self->item_lengths + i, 1);
        if (self->items[i] == NULL) { PyErr_NoMemory(); goto end; }
    }

end:
    Py_XDECREF(py_items);
    if (PyErr_Occurred()) { free_matcher(self); }
    return (PyErr_Occurred()) ? -1 : 0;
}
// Matcher.__init__() }}}
 
// Matcher.calculate_scores {{{
static PyObject *
Matcher_calculate_scores(Matcher *self, PyObject *args) {
    int32_t *final_positions = NULL, *p;
    Match *matches = NULL;
    bool ok = FALSE;
    uint32_t i = 0, needle_char_len = 0, j = 0;
    PyObject *items = NULL, *score = NULL, *positions = NULL, *pneedle = NULL;
    UChar *needle = NULL;

    if (!PyArg_ParseTuple(args, "O", &pneedle)) return NULL;

    needle = python_to_icu(pneedle, NULL, 1);
    if (needle == NULL) return NULL;
    needle_char_len = u_countChar32(needle, -1);
    items = PyTuple_New(self->item_count);
    positions = PyTuple_New(self->item_count);
    matches = (Match*)calloc(self->item_count, sizeof(Match));
    final_positions = (int32_t*) calloc(needle_char_len * self->item_count, sizeof(int32_t));
    if (items == NULL || matches == NULL || final_positions == NULL || positions == NULL) {PyErr_NoMemory(); goto end;}

    for (i = 0; i < self->item_count; i++) {
        score = PyTuple_New(needle_char_len);
        if (score == NULL) { PyErr_NoMemory(); goto end; }
        PyTuple_SET_ITEM(positions, (Py_ssize_t)i, score);
    }

    Py_BEGIN_ALLOW_THREADS;
    ok = match(self->items, self->item_lengths, self->item_count, needle, matches, final_positions, needle_char_len, self->level1, self->level2, self->level3);
    Py_END_ALLOW_THREADS;

    if (ok) {
        for (i = 0; i < self->item_count; i++) {
            score = PyFloat_FromDouble(matches[i].score);
            if (score == NULL) { PyErr_NoMemory(); goto end; }
            PyTuple_SET_ITEM(items, (Py_ssize_t)i, score);
            p = final_positions + i;
            for (j = 0; j < needle_char_len; j++) {
                score = PyInt_FromLong((long)p[j]);
                if (score == NULL) { PyErr_NoMemory(); goto end; }
                PyTuple_SET_ITEM(PyTuple_GET_ITEM(positions, (Py_ssize_t)i), (Py_ssize_t)j, score);
            }
        }
    } else { PyErr_NoMemory(); goto end; }

end:
    nullfree(needle);
    nullfree(matches);
    nullfree(final_positions);
    if (PyErr_Occurred()) { Py_XDECREF(items); items = NULL; Py_XDECREF(positions); positions = NULL; return NULL; }
    return Py_BuildValue("NN", items, positions);
} // }}}

static PyMethodDef Matcher_methods[] = {
    {"calculate_scores", (PyCFunction)Matcher_calculate_scores, METH_VARARGS,
     "calculate_scores(query) -> Return the scores for all items given query as a tuple."
    },

    {NULL}  /* Sentinel */
};


// }}}

static PyTypeObject MatcherType = { // {{{
    PyObject_HEAD_INIT(NULL)
    0,                         /*ob_size*/
    "matcher.Matcher",            /*tp_name*/
    sizeof(Matcher),      /*tp_basicsize*/
    0,                         /*tp_itemsize*/
    (destructor)Matcher_dealloc, /*tp_dealloc*/
    0,                         /*tp_print*/
    0,                         /*tp_getattr*/
    0,                         /*tp_setattr*/
    0,                         /*tp_compare*/
    0,                         /*tp_repr*/
    0,                         /*tp_as_number*/
    0,                         /*tp_as_sequence*/
    0,                         /*tp_as_mapping*/
    0,                         /*tp_hash */
    0,                         /*tp_call*/
    0,                         /*tp_str*/
    0,                         /*tp_getattro*/
    0,                         /*tp_setattro*/
    0,                         /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT|Py_TPFLAGS_BASETYPE,        /*tp_flags*/
    "Matcher",                  /* tp_doc */
    0,		               /* tp_traverse */
    0,		               /* tp_clear */
    0,		               /* tp_richcompare */
    0,		               /* tp_weaklistoffset */
    0,		               /* tp_iter */
    0,		               /* tp_iternext */
    Matcher_methods,             /* tp_methods */
    0,             /* tp_members */
    0,                         /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    (initproc)Matcher_init,      /* tp_init */
    0,                         /* tp_alloc */
    0,                 /* tp_new */
}; // }}}

static PyMethodDef matcher_methods[] = {
    {NULL, NULL, 0, NULL}
};


PyMODINIT_FUNC
initmatcher(void) {
    PyObject *m;
    MatcherType.tp_new = PyType_GenericNew;
    if (PyType_Ready(&MatcherType) < 0)
        return;
    m = Py_InitModule3("matcher", matcher_methods, "Find subsequence matches");
    if (m == NULL) return;

    Py_INCREF(&MatcherType);
    PyModule_AddObject(m, "Matcher", (PyObject *)&MatcherType);

}


