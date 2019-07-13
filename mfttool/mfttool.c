/*
 * Copyright (c) 2015-2019 Contributors as noted in the AUTHORS file
 *
 * This file is part of Solo5, a sandboxed execution environment.
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose with or without fee is hereby granted, provided
 * that the above copyright notice and this permission notice appear
 * in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * mfttool.c: Solo5 application manifest generator.
 *
 * This tool produces a C source file defining the binary manifest from its
 * JSON source. The produced C source file should be compiled with the Solo5
 * toolchain and linked into the unikernel binary.
 */

#define _GNU_SOURCE
#include <assert.h>
#include <ctype.h>
#include <err.h>
#include <libgen.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "json.h"
#include "mft_abi.h"

/*
 * For "dump" functionality, we pull in the ELF loader and manifest validation
 * code directly to simplify the build.
 */
#include "../tenders/common/mft.c"
#include "../tenders/common/elf.c"

static const char *jtypestr(enum jtypes t)
{
    switch (t) {
    case jnull:     return "NULL";
    case jtrue:     return "BOOLEAN";
    case jfalse:    return "BOOLEAN";
    case jstring:   return "STRING";
    case jarray:    return "ARRAY";
    case jobject:   return "OBJECT";
    case jint:      return "INTEGER";
    case jreal:     return "REAL";
    default:        return "UNKNOWN";
    }
}

static void jexpect(enum jtypes t, jvalue *v, const char *loc)
{
    if (v->d != t)
        errx(1, "%s: expected %s, got %s", loc, jtypestr(t), jtypestr(v->d));
}

static const char out_header[] = \
    "#define MFT_ENTRIES %"PRIu32"\n"
    "#include \"mft_abi.h\"\n"
    "\n"
    "MFT_NOTE_BEGIN\n"
    "{\n"
    "  .version = %"PRIu32", .entries = %"PRIu32",\n"
    "  .e = {\n";

static const char out_entry[] = \
    "    { .name = \"%s\", .type = MFT_%s },\n";

static const char out_footer[] = \
    "  }\n"
    "}\n"
    "MFT_NOTE_END\n";

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s COMMAND ...\n\n", prog);
    fprintf(stderr, "COMMAND is:\n");
    fprintf(stderr, "    dump BINARY:\n");
    fprintf(stderr, "        Dump the application manifest from BINARY.\n");
    fprintf(stderr, "    dump-json BINARY:\n");
    fprintf(stderr, "        Dump application manifest from BINARY.\n");
    fprintf(stderr, "    dump-c BINARY OUTPUT:\n");
    fprintf(stderr, "        Dump application manifest from BINARY.\n");
    fprintf(stderr, "    gen SOURCE OUTPUT:\n");
    fprintf(stderr, "        Generate application manifest from SOURCE, "
            "writing to OUTPUT.\n");
    exit(EXIT_FAILURE);
}


/*
 * Parses a JSON file from the input stream into an MFT struct.
 * The returned struct will be allocated with malloc() and thus
 * will need to be free()'ed.
 */
static void
mfttool_mft_of_json(
    const char *input_name, FILE *sfp, struct mft **ret_mft)
{
    if (ret_mft == NULL)
        errx(1, "struct mft *dest is NULL");

    jvalue *root = jparse(sfp);
    fclose(sfp);

    if (root == NULL)
        errx(1, "JSON parse error");
    jupdate(root);
    jexpect(jobject, root, "(root)");

    jvalue *jversion = NULL, *jdevices = NULL;
    size_t entries = 0;

    /* find version in the JSON file;
     * register other keys regardless of MFT ABI version */
    for(jvalue **i = root->u.v; *i; ++i) {
        if (strcmp((*i)->n, "version") == 0) {
            jexpect(jint, *i, ".version");
            jversion = *i;
        } else if (strcmp((*i)->n, "devices") == 0) {
            jdevices = *i;
        } else /* TODO fail when we know ABI version so we can give meaningful error */
            errx(1, "(root): unknown key: %s", (*i)->n);
    }
    if (jversion == NULL)
        errx(1, "missing .version");

    /* skeleton code for supporting multiple ABIs */
    _Static_assert((MFT_VERSION <= 1),
        "mfttool.c needs to be made to support current MFT schema");
    switch (jversion->u.i) {
    case 1:
        jexpect(jarray, jdevices, ".devices");
        break;
    default:
        errx(1, "(root): MFT version %llu not supported\n", jversion->u.i);
    }


    /* Since we currently only have one version,
     * everything below is currently to MFT ABI v1 */


    /* We have to traverse the device list twice because we do not
     * presently know how many entries to expect, and therefore cannot
     * allocate memory for them. */
    for(jvalue **i = jdevices->u.v; *i; ++i) {
        jexpect(jobject, *i, ".devices[]");
        entries++;
    }

    if (entries > MFT_MAX_ENTRIES)
        errx(1, "%s: .devices[]: too many entries, "
            "maximum %"PRIu32, input_name, MFT_MAX_ENTRIES);

    size_t mft_size =
        sizeof(struct mft) + entries * sizeof(struct mft_entry);
    struct mft *mft = calloc(1, mft_size);
    if (mft == NULL)
        errx(1, "out of memory");

    mft->version = jversion->u.i;
    mft->entries = entries;

    size_t idx = 0;
    for (jvalue **i = jdevices->u.v; *i; ++i) {
        jexpect(jobject, *i, ".devices[]");
        mft->e[idx].type = -1; /* TODO ideally an enum value too? -1 to not default to MFT_BLOCK_BASIC if unset */
        char *v_str = NULL;
        for (jvalue **j = (*i)->u.v; *j; ++j) {
            jexpect(jstring, *j, ".devices[...]");
            v_str = (*j)->u.s;
            if (strcmp((*j)->n, "name") == 0 && v_str) {
                strncpy((mft->e[idx].name), v_str, MFT_NAME_SIZE);
            }
            else if (strcmp((*j)->n, "type") == 0) {
                if (mft_string_to_type(v_str, &mft->e[idx].type))
                    errx(1, ".device[%zd]: unknown 'type'", idx);
            }
            else
                errx(1, ".devices[%zd]: unknown key: %s", idx, (*j)->n);
        }
        idx++;
    }

    jdel(root); /* free json object */

    if (mft_validate(mft, mft_size)) {
        errx(1, "%s: Manifest validation failed", input_name);
    }

    *ret_mft = mft;
}

static struct mft *
mfttool_mft_of_binary_path(const char *input_path)
{
    struct mft *mft;
    size_t mft_size;
    elf_load_mft(input_path, &mft, &mft_size);
    if (mft_validate(mft, mft_size))
        errx(1, "%s: Manifest validation failed", input_path);
    return mft;
}

static int
mfttool_output_c(const struct mft *mft, FILE *ofp)
{
    _Static_assert((MFT_VERSION <= 1),
        "Please update mfttool.c:mfttool_generate with new ABI support");
    switch (mft->version){
    case 1:
        break;
    default:
        errx(1, "This version of mfttool is too outdated to handle"
            "MFT ABI version %"PRIu32"\n", mft->version);
        return EXIT_FAILURE;
    }

    fprintf(ofp, out_header, mft->entries, mft->version, mft->entries);

    for(int i = 0; i != mft->entries; i++) {
    fprintf(ofp, out_entry, mft->e[i].name,
        mft_type_to_string(mft->e[i].type));
    }

    fprintf(ofp, out_footer);
    fflush(ofp);
    return EXIT_SUCCESS;
}

static int
mfttool_output_json(const struct mft *mft, FILE *ofp)
{
    _Static_assert((MFT_VERSION <= 1), "TODO");
    switch (mft->version) {
    case 1:
        /* schema for v1:
         * { .version = jint(mft.version),
         *   .devices = jlist [ { .name = jstring,
         *                        .type = jstring }], }
         */
        fprintf(ofp, "{ \"version\": %"PRIu32",\n", mft->version);
        fprintf(ofp, "  \"devices\": [");
        for (unsigned i = 0; i != mft->entries; i++) {
            if (i) fprintf(ofp, "\n              ");
            fprintf(ofp, " { \"type\": \"%s\", ",
                mft_type_to_string(mft->e[i].type));
            fprintf(ofp, "\"name\": \"%s\" }", mft->e[i].name);
            if (i != mft->entries - 1) fprintf(ofp, ",");
        }
        fprintf(ofp, " ] }\n");
        break;
    default:
            warnx("This version of mfttool is too outdated to handle"
                  "MFT ABI version %"PRIu32"\n", mft->version);
            return EXIT_FAILURE;
    }

    fflush(ofp);
    return EXIT_SUCCESS;
}

int main(int argc, char *argv[])
{
    const char *prog;
    FILE *input = stdin;
    FILE *output = stdout;
    char *input_path = NULL;

    prog = basename(argv[0]);

    enum { USAGE, ELF_TO_C, ELF_TO_JSON, JSON_TO_C, } subcommand = USAGE;

    if (argc < 2 || 4 < argc) {
        /* print usage and abort */
    }
    else if (strcmp(argv[1], "gen") == 0 && argc >= 3) {
        subcommand = JSON_TO_C;
    }
    else if (strcmp(argv[1], "dump") == 0 && argc >= 3) {
        subcommand = ELF_TO_JSON; /*elf2json*/
    }
    else if (strcmp(argv[1], "elf2c") == 0 && argc >= 3) {
        subcommand = ELF_TO_C;
    }

    if (USAGE != subcommand) {
        /* all subcommands take input file as first arg: */
        input_path = argv[2];

        if (strcmp(input_path, "-") != 0)
            input = fopen(input_path, "r");

        if (!input)
            err(EXIT_FAILURE, "%s (input file)", input_path);

       /* All subcommands currently take output as second arg
        * Default to stdout if not given:
        */
        char *output_path = NULL;
        if (argc >= 4) {
           output_path = argv[3];

        if (strcmp(output_path, "-") != 0)
            output = fopen(output_path, "w");
        }

        if (!output)
            err(1, "%s", output_path);
    }

    struct mft *mft;
    switch (subcommand) {
    case ELF_TO_JSON:
        mft = mfttool_mft_of_binary_path(input_path);
        return mfttool_output_json(mft, output);
    case JSON_TO_C:
        mfttool_mft_of_json(input_path, input, &mft);
        return mfttool_output_c(mft, output);
    case ELF_TO_C:
        mft = mfttool_mft_of_binary_path(input_path);
        return mfttool_output_c(mft, output);
    case USAGE: /* fallthru */
    default:
        usage(prog);
    }
}
