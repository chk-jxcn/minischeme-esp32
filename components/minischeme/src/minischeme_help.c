/**
 * @file minischeme_help.c
 * @brief Interactive documentation, built-in function reference, and REPL autocompletion.
 * 
 * Compile-time Toggle:
 *   Define MINISCHEME_NO_HELP to completely strip the documentation table and autocompletion
 *   engine from the binary for minimal flash/RAM footprint on production/embedded targets (e.g. ESP32).
 */

#include "minischeme.h"
#include "minischeme_internal.h"
#include "minischeme_help.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MINISCHEME_NO_HELP

/* ========================================================================= */
/* Built-in Help and Reference Metadata Table                                */
/* ========================================================================= */

/* Generated from the API specification (api/spec, tools/gen_api.py): the
 * entries of every implemented API procedure, special forms included */
#include "minischeme_help_table.h"

static const MiniHelpEntry HELP_TABLE[] = {
    MINI_HELP_TABLE_ENTRIES
};

const MiniHelpEntry *mini_help_lookup(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; HELP_TABLE[i].name != NULL; i++) {
        if (strcmp(HELP_TABLE[i].name, name) == 0) {
            return &HELP_TABLE[i];
        }
    }
    return NULL;
}

void mini_print_help(MiniVM *vm, const char *symbol, FILE *stream) {
    (void)vm;
    if (!stream) stream = stdout;

    if (symbol && strlen(symbol) > 0) {
        const MiniHelpEntry *entry = mini_help_lookup(symbol);
        if (entry) {
            fprintf(stream, "\n============================================================\n");
            fprintf(stream, "  Name:        %s\n", entry->name);
            fprintf(stream, "  Syntax:      %s\n", entry->syntax);
            fprintf(stream, "  Category:    %s\n", entry->category);
            fprintf(stream, "  Description: %s\n", entry->description);
            fprintf(stream, "============================================================\n");
            return;
        }
        fprintf(stream, "No documentation found for '%s'.\nType (help) to list all available standard functions.\n", symbol);
        return;
    }

    fprintf(stream, "\n============================================================\n");
    fprintf(stream, "            MiniScheme Built-in Functions & Reference       \n");
    fprintf(stream, "============================================================\n");
    for (size_t i = 0; HELP_TABLE[i].name != NULL; i++) {
        fprintf(stream, "  %-32s - %s\n", HELP_TABLE[i].syntax, HELP_TABLE[i].description);
    }
}

Value prim_help(MiniVM *vm, int argc, Value *argv) {
    if (argc == 0) {
        mini_print_help(vm, NULL, stdout);
        return mini_void();
    }
    if (argc == 1) {
        Value target = argv[0];
        if (mini_is_pair(target) && mini_is_pair(mini_cdr(target))) {
            Value q = mini_car(target);
            if (mini_is_symbol(q) && strcmp(mini_symbol_name(q), "quote") == 0) {
                target = mini_car(mini_cdr(target));
            }
        }
        const char *name = NULL;
        if (mini_is_symbol(target)) name = mini_symbol_name(target);
        else if (mini_is_string(target)) name = mini_string_data(target);
        else {
            vm_error(vm, "help: expected symbol or string, got other type");
            return mini_void();
        }
        mini_print_help(vm, name, stdout);
        return mini_void();
    }
    vm_error(vm, "help: expected 0 or 1 argument, got %d", argc);
    return mini_void();
}

Value prim_help_categories(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    static const char *cats[] = { MINI_HELP_CATEGORY_LIST };
    Value res = mini_nil();
    mini_push_root(vm, &res);
    for (int i = MINI_HELP_CATEGORY_COUNT - 1; i >= 0; i--) {
        Value s = mini_make_string(vm, cats[i]);
        res = mini_make_cons(vm, s, res);
    }
    mini_pop_root(vm);
    return res;
}

Value prim_help_lookup(MiniVM *vm, int argc, Value *argv) {
    if (argc < 1 || !mini_is_string(argv[0])) return mini_false();
    const char *name = mini_string_data(argv[0]);
    const MiniHelpEntry *e = mini_help_lookup(name);
    if (!e) return mini_false();
    Value v_name = mini_make_string(vm, e->name);
    mini_push_root(vm, &v_name);
    Value v_syn = mini_make_string(vm, e->syntax);
    mini_push_root(vm, &v_syn);
    Value v_cat = mini_make_string(vm, e->category);
    mini_push_root(vm, &v_cat);
    Value v_desc = mini_make_string(vm, e->description);
    mini_push_root(vm, &v_desc);

    Value res = mini_make_cons(vm, v_desc, mini_nil());
    res = mini_make_cons(vm, v_cat, res);
    res = mini_make_cons(vm, v_syn, res);
    res = mini_make_cons(vm, v_name, res);
    mini_pop_root(vm);
    mini_pop_root(vm);
    mini_pop_root(vm);
    mini_pop_root(vm);
    return res;
}

/* Categories are "Group / Component" (api/components.py). A query matches
 * the whole name, a group ("Hardware": every "Hardware / ..."), or one of
 * the category names before API 1.0.0 (init.scm's (functions 'hardware)) */
static bool help_category_matches(const char *category, const char *want) {
    static const char *const legacy[][3] = {
        { "Hardware & Peripherals", "Hardware", NULL },
        { "Math & Numeric", "Language / Numbers", NULL },
        { "Pairs & Lists", "Language / Pairs and lists", NULL },
        { "Vectors", "Language / Vectors", NULL },
        { "Strings & Characters", "Language / Strings", "Language / Characters" },
        { "Predicates & Types", "Language / Types and equivalence", NULL },
        { "I/O & System", "System", "Language / Output and loading" },
        { "Control Flow & Macros", "Language / Control", NULL },
        { "Special Forms", "Language / Special forms", NULL },
    };
    for (size_t i = 0; i < sizeof(legacy) / sizeof(legacy[0]); i++) {
        if (strcmp(want, legacy[i][0]) == 0) {
            return help_category_matches(category, legacy[i][1]) ||
                   (legacy[i][2] && help_category_matches(category, legacy[i][2]));
        }
    }
    size_t n = strlen(want);
    return strncmp(category, want, n) == 0 && (category[n] == '\0' || strncmp(category + n, " / ", 3) == 0);
}

Value prim_help_entries_for_category(MiniVM *vm, int argc, Value *argv) {
    if (argc < 1 || !mini_is_string(argv[0])) return mini_nil();
    char cat[64];   /* a copy: the allocations below may move the string */
    snprintf(cat, sizeof(cat), "%s", mini_string_data(argv[0]));
    Value list = mini_nil();
    mini_push_root(vm, &list);
    int count = 0;
    for (; HELP_TABLE[count].name != NULL; count++);
    for (int i = count - 1; i >= 0; i--) {
        if (help_category_matches(HELP_TABLE[i].category, cat)) {
            Value v_name = mini_make_string(vm, HELP_TABLE[i].name);
            mini_push_root(vm, &v_name);
            Value v_syn = mini_make_string(vm, HELP_TABLE[i].syntax);
            mini_push_root(vm, &v_syn);
            Value v_cat = mini_make_string(vm, HELP_TABLE[i].category);
            mini_push_root(vm, &v_cat);
            Value v_desc = mini_make_string(vm, HELP_TABLE[i].description);
            mini_push_root(vm, &v_desc);

            Value entry = mini_make_cons(vm, v_desc, mini_nil());
            entry = mini_make_cons(vm, v_cat, entry);
            entry = mini_make_cons(vm, v_syn, entry);
            entry = mini_make_cons(vm, v_name, entry);
            mini_pop_root(vm);
            mini_pop_root(vm);
            mini_pop_root(vm);
            mini_pop_root(vm);

            list = mini_make_cons(vm, entry, list);
        }
    }
    mini_pop_root(vm);
    return list;
}

Value prim_help_names_for_category(MiniVM *vm, int argc, Value *argv) {
    if (argc < 1 || !mini_is_string(argv[0])) return mini_nil();
    char cat[64];
    snprintf(cat, sizeof(cat), "%s", mini_string_data(argv[0]));
    Value list = mini_nil();
    mini_push_root(vm, &list);
    int count = 0;
    for (; HELP_TABLE[count].name != NULL; count++);
    for (int i = count - 1; i >= 0; i--) {
        if (help_category_matches(HELP_TABLE[i].category, cat)) {
            Value sym = mini_make_symbol(vm, HELP_TABLE[i].name);
            list = mini_make_cons(vm, sym, list);
        }
    }
    mini_pop_root(vm);
    return list;
}

/* ========================================================================= */
/* Autocompletion Engine                                                     */
/* ========================================================================= */

static int compare_completion_strings(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

static void add_completion(char ***matches_ptr, size_t *count_ptr, size_t *cap_ptr, const char *name) {
    char **matches = *matches_ptr;
    for (size_t j = 0; j < *count_ptr; j++) {
        if (strcmp(matches[j], name) == 0) return;
    }
    if (*count_ptr >= *cap_ptr) {
        size_t new_cap = (*cap_ptr) * 2;
        char **new_m = (char **)realloc(matches, new_cap * sizeof(char *));
        if (!new_m) return;
        *matches_ptr = new_m;
        *cap_ptr = new_cap;
        matches = new_m;
    }
    matches[*count_ptr] = vm_strdup(name);
    if (matches[*count_ptr]) (*count_ptr)++;
}

int mini_get_completions(MiniVM *vm, const char *prefix, char ***out_matches) {
    if (!out_matches) return 0;
    *out_matches = NULL;
    if (!prefix) prefix = "";

    size_t prefix_len = strlen(prefix);
    size_t cap = 64;
    size_t count = 0;
    char **matches = (char **)malloc(cap * sizeof(char *));
    if (!matches) return 0;

    /* 1. Collect from HELP_TABLE */
    for (size_t i = 0; HELP_TABLE[i].name != NULL; i++) {
        if (strncmp(HELP_TABLE[i].name, prefix, prefix_len) == 0) {
            add_completion(&matches, &count, &cap, HELP_TABLE[i].name);
        }
    }

    /* 2. Collect from vm->global_env if vm is available */
    if (vm && !mini_is_nil(vm->global_env)) {
        for (Value curr = vm->global_env; mini_is_pair(curr); curr = mini_cdr(curr)) {
            Value binding = mini_car(curr);
            if (mini_is_pair(binding) && mini_is_symbol(mini_car(binding))) {
                const char *sname = mini_symbol_name(mini_car(binding));
                if (strncmp(sname, prefix, prefix_len) == 0) {
                    add_completion(&matches, &count, &cap, sname);
                }
            }
        }
    }

    /* 2b. Primitives from static tables that are not bound yet */
    if (vm) {
        for (int t = 0; t < vm->prim_table_count; t++) {
            for (size_t i = 0; i < vm->prim_table_sizes[t]; i++) {
                const char *pname = vm->prim_tables[t][i].name;
                if (strncmp(pname, prefix, prefix_len) == 0) {
                    add_completion(&matches, &count, &cap, pname);
                }
            }
        }
    }

    /* 3. Sort matches alphabetically */
    if (count > 1) {
        qsort(matches, count, sizeof(char *), compare_completion_strings);
    }

    *out_matches = matches;
    return (int)count;
}

void mini_free_completions(char **matches, int count) {
    if (!matches) return;
    for (int i = 0; i < count; i++) {
        free(matches[i]);
    }
    free(matches);
}

void mini_register_help_lib(MiniVM *vm) {
    static const MiniPrimEntry funcs[] = {
        { "help",                       prim_help },
        { "%help",                      prim_help },
        { "%help-lookup",               prim_help_lookup },
        { "%help-categories",           prim_help_categories },
        { "%help-entries-for-category", prim_help_entries_for_category },
        { "%help-names-for-category",   prim_help_names_for_category },
    };
    mini_register_prim_table(vm, funcs, sizeof(funcs) / sizeof(funcs[0]));
}

#else /* =================================================================== */
/* MINISCHEME_NO_HELP defined: Zero-Footprint Stubs for Production Release   */
/* ========================================================================= */

const MiniHelpEntry *mini_help_lookup(const char *name) {
    (void)name;
    return NULL;
}

void mini_print_help(MiniVM *vm, const char *symbol, FILE *stream) {
    (void)vm; (void)symbol;
    if (!stream) stream = stdout;
    fprintf(stream, "Help system is disabled in this release build.\n");
}

Value prim_help(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    mini_print_help(vm, NULL, stdout);
    return mini_void();
}

Value prim_help_categories(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    return mini_nil();
}

Value prim_help_lookup(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    return mini_false();
}

Value prim_help_entries_for_category(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    return mini_nil();
}

Value prim_help_names_for_category(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    return mini_nil();
}

int mini_get_completions(MiniVM *vm, const char *prefix, char ***out_matches) {
    (void)vm; (void)prefix;
    if (out_matches) *out_matches = NULL;
    return 0;
}

void mini_free_completions(char **matches, int count) {
    (void)matches; (void)count;
}

void mini_register_help_lib(MiniVM *vm) {
    static const MiniPrimEntry funcs[] = {
        { "help",                       prim_help },
        { "%help",                      prim_help },
        { "%help-lookup",               prim_help_lookup },
        { "%help-categories",           prim_help_categories },
        { "%help-entries-for-category", prim_help_entries_for_category },
        { "%help-names-for-category",   prim_help_names_for_category },
    };
    mini_register_prim_table(vm, funcs, sizeof(funcs) / sizeof(funcs[0]));
}

#endif /* MINISCHEME_NO_HELP */
