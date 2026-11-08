/*
 * bench_gen.h — generated benchmark inputs, from Oyl's bench/bench_gen.h
 *
 * The same four shapes as Oyl's, written as YAML 2.0: each starts with
 * %YAML 2.0, so the text after that first line is the YAML 1.2 that Oyl
 * reads, and gives the same events. The config input has no anchors,
 * since 2.0 has none; config_table is the same data with its labels in
 * the table, which only 2.0 can read.
 */

#ifndef OYLNG_BENCH_GEN_H
#define OYLNG_BENCH_GEN_H

#include <stdio.h>
#include <stdlib.h>

/* Mixed: mappings, sequences, flow collections, quoted strings */
static char *gen_mixed(size_t target, size_t *len) {
    char *buf = malloc(target + 4096);
    if (!buf) return NULL;
    size_t pos = 0;
    pos += sprintf(buf + pos, "%%YAML 2.0\n---\nmetadata:\n  name: benchmark-data\n"
                              "  version: 1.2.0\n  generated: true\nentries:\n");
    for (int item = 0; pos < target; item++)
        pos += sprintf(buf + pos,
            "  - id: %d\n"
            "    name: \"item-%d\"\n"
            "    value: %d.%02d\n"
            "    tags: [alpha, beta, gamma]\n"
            "    nested:\n"
            "      x: %d\n"
            "      y: %d\n"
            "      label: 'entry #%d'\n",
            item, item, item * 17 % 1000, item * 31 % 100,
            item * 7 % 500, item * 13 % 500, item);
    pos += sprintf(buf + pos, "...\n");
    *len = pos;
    return buf;
}

/* Block: only mappings and sequences, no flow or quotes */
static char *gen_block(size_t target, size_t *len) {
    char *buf = malloc(target + 4096);
    if (!buf) return NULL;
    size_t pos = sprintf(buf, "%%YAML 2.0\n---\n");
    for (int item = 0; pos < target; item++)
        pos += sprintf(buf + pos,
            "- id: %d\n"
            "  name: item-%d\n"
            "  value: %d\n"
            "  nested:\n"
            "    x: %d\n"
            "    y: %d\n",
            item, item, item * 17 % 1000, item * 7 % 500, item * 13 % 500);
    *len = pos;
    return buf;
}

/* JSON: an array of objects with quoted keys and strings */
static char *gen_json(size_t target, size_t *len) {
    char *buf = malloc(target + 4096);
    if (!buf) return NULL;
    size_t pos = sprintf(buf, "%%YAML 2.0\n---\n[\n");
    for (int item = 0; pos < target; item++) {
        if (item > 0) pos += sprintf(buf + pos, ",\n");
        pos += sprintf(buf + pos,
            "  {\"id\": %d, \"name\": \"item-%d\", \"value\": %d,"
            " \"nested\": {\"x\": %d, \"y\": %d}}",
            item, item, item * 17 % 1000, item * 7 % 500, item * 13 % 500);
    }
    pos += sprintf(buf + pos, "\n]\n");
    *len = pos;
    return buf;
}

/* Config: many documents with comments, block scalars and flow, as in CI
 * pipelines and deployment manifests. With table, the labels are a table
 * entry each document names twice. */
static char *gen_config_as(size_t target, size_t *len, int table) {
    char *buf = malloc(target + 4096);
    if (!buf) return NULL;
    size_t pos = sprintf(buf, "%%YAML 2.0\n");
    for (int item = 0; pos < target; item++) {
        if (table)
            pos += sprintf(buf + pos,
                "--- &\n"
                "labels:\n"
                "  app: svc-%d\n"
                "  tier: backend\n", item);
        pos += sprintf(buf + pos,
            "# service %d\n"
            "---\n"
            "apiVersion: apps/v1\n"
            "kind: Deployment\n"
            "metadata:\n"
            "  name: svc-%d  # generated\n", item, item);
        if (table)
            pos += sprintf(buf + pos, "  labels: *labels\nspec:\n  replicas: %d\n"
                                      "  selector:\n    matchLabels: *labels\n", item % 5 + 1);
        else
            pos += sprintf(buf + pos,
                "  labels:\n    app: svc-%d\n    tier: backend\n"
                "spec:\n  replicas: %d\n  selector:\n    matchLabels:\n"
                "      app: svc-%d\n      tier: backend\n", item, item % 5 + 1, item);
        pos += sprintf(buf + pos,
            "  template:\n"
            "    spec:\n"
            "      containers:\n"
            "        - name: main\n"
            "          image: \"registry.example.com/svc-%d:1.%d.%d\"\n"
            "          args: [\"--port\", \"80%02d\", \"--verbose\"]\n"
            "          env:\n"
            "            - name: MODE\n"
            "              value: 'production'\n"
            "          command:\n"
            "            - /bin/sh\n"
            "            - -c\n"
            "            - |\n"
            "              echo starting svc-%d\n"
            "              exec /app/server --workers %d\n"
            "          description: >\n"
            "            Folded text for service %d that\n"
            "            continues over two lines.\n",
            item, item % 10, item % 7, item % 100, item, item % 16 + 1, item);
    }
    *len = pos;
    return buf;
}

#endif
