/* Build configuration for the Expat copy the Reader uses to parse EPUBs.
 * Upstream generates this file with CMake or configure; neither runs here.
 *
 * XML_GE=0 and 1 KB of context are what CrossPoint Reader builds Expat with.
 * Without general entities an HTML entity such as &nbsp; reaches the default
 * handler instead of failing the parse, which is where epub.c expands it. */
#ifndef EXPAT_CONFIG_H
#define EXPAT_CONFIG_H 1

#define BYTEORDER 1234
#define XML_CONTEXT_BYTES 1024
#define XML_GE 0

/* epub.c gives every parser its salt with XML_SetHashSalt(), so Expat never
 * gathers entropy of its own and its weak fallback is never reached. */
#define XML_POOR_ENTROPY 1

#endif
