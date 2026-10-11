/*
 * SPDX-License-Identifier: MIT
 * ThistleOS — the HTML named entities an EPUB chapter may use.
 *
 * The table is CrossPoint Reader's htmlEntities.cpp, Copyright (c) 2025 Dave
 * Allie, which took it from atomic14's diy-esp32-epub-reader, Copyright (c)
 * Chris Greening. Both MIT; see THIRD_PARTY_LICENSES.md.
 */
#include "reader/html_entities.h"

#include <string.h>

typedef struct {
    const char *key;
    const char *value;
} entity_t;

/* Sorted by key, byte by byte, for the binary search below; a test checks it. */
static const entity_t ENTITIES[] = {
    {"&AElig;", "Æ"},    {"&Aacute;", "Á"},     {"&Acirc;", "Â"},      {"&Agrave;", "À"},   {"&Alpha;", "Α"},
    {"&Aring;", "Å"},    {"&Atilde;", "Ã"},     {"&Auml;", "Ä"},       {"&Beta;", "Β"},     {"&Ccedil;", "Ç"},
    {"&Chi;", "Χ"},      {"&Dagger;", "‡"},     {"&Delta;", "Δ"},      {"&ETH;", "Ð"},      {"&Eacute;", "É"},
    {"&Ecirc;", "Ê"},    {"&Egrave;", "È"},     {"&Epsilon;", "Ε"},    {"&Eta;", "Η"},      {"&Euml;", "Ë"},
    {"&Gamma;", "Γ"},    {"&Iacute;", "Í"},     {"&Icirc;", "Î"},      {"&Igrave;", "Ì"},   {"&Iota;", "Ι"},
    {"&Iuml;", "Ï"},     {"&Kappa;", "Κ"},      {"&Lambda;", "Λ"},     {"&Mu;", "Μ"},       {"&Ntilde;", "Ñ"},
    {"&Nu;", "Ν"},       {"&OElig;", "Œ"},      {"&Oacute;", "Ó"},     {"&Ocirc;", "Ô"},    {"&Ograve;", "Ò"},
    {"&Omega;", "Ω"},    {"&Omicron;", "Ο"},    {"&Oslash;", "Ø"},     {"&Otilde;", "Õ"},   {"&Ouml;", "Ö"},
    {"&Phi;", "Φ"},      {"&Pi;", "Π"},         {"&Prime;", "″"},      {"&Psi;", "Ψ"},      {"&Rho;", "Ρ"},
    {"&Scaron;", "Š"},   {"&Sigma;", "Σ"},      {"&THORN;", "Þ"},      {"&Tau;", "Τ"},      {"&Theta;", "Θ"},
    {"&Uacute;", "Ú"},   {"&Ucirc;", "Û"},      {"&Ugrave;", "Ù"},     {"&Upsilon;", "Υ"},  {"&Uuml;", "Ü"},
    {"&Xi;", "Ξ"},       {"&Yacute;", "Ý"},     {"&Yuml;", "Ÿ"},       {"&Zeta;", "Ζ"},     {"&aacute;", "á"},
    {"&acirc;", "â"},    {"&acute;", "´"},      {"&aelig;", "æ"},      {"&agrave;", "à"},   {"&alpha;", "α"},
    {"&amp;", "&"},      {"&and;", "∧"},        {"&ang;", "∠"},        {"&aring;", "å"},    {"&asymp;", "≈"},
    {"&atilde;", "ã"},   {"&auml;", "ä"},       {"&bdquo;", "„"},      {"&beta;", "β"},     {"&brvbar;", "¦"},
    {"&bull;", "•"},     {"&cap;", "∩"},        {"&ccedil;", "ç"},     {"&cedil;", "¸"},    {"&cent;", "¢"},
    {"&chi;", "χ"},      {"&circ;", "ˆ"},       {"&clubs;", "♣"},      {"&cong;", "≅"},     {"&copy;", "©"},
    {"&crarr;", "↵"},    {"&cup;", "∪"},        {"&curren;", "¤"},     {"&dagger;", "†"},   {"&darr;", "↓"},
    {"&deg;", "°"},      {"&delta;", "δ"},      {"&diams;", "♦"},      {"&divide;", "÷"},   {"&eacute;", "é"},
    {"&ecirc;", "ê"},    {"&egrave;", "è"},     {"&empty;", "∅"},      {"&emsp;", " "},     {"&ensp;", " "},
    {"&epsilon;", "ε"},  {"&equiv;", "≡"},      {"&eta;", "η"},        {"&eth;", "ð"},      {"&euml;", "ë"},
    {"&euro;", "€"},     {"&exist;", "∃"},      {"&fnof;", "ƒ"},       {"&forall;", "∀"},   {"&frac12;", "½"},
    {"&frac14;", "¼"},   {"&frac34;", "¾"},     {"&frasl;", "⁄"},      {"&gamma;", "γ"},    {"&ge;", "≥"},
    {"&gt;", ">"},       {"&harr;", "↔"},       {"&hearts;", "♥"},     {"&hellip;", "…"},   {"&iacute;", "í"},
    {"&icirc;", "î"},    {"&iexcl;", "¡"},      {"&igrave;", "ì"},     {"&infin;", "∞"},    {"&int;", "∫"},
    {"&iota;", "ι"},     {"&iquest;", "¿"},     {"&isin;", "∈"},       {"&iuml;", "ï"},     {"&kappa;", "κ"},
    {"&lambda;", "λ"},   {"&laquo;", "«"},      {"&larr;", "←"},       {"&lceil;", "⌈"},    {"&ldquo;", "\u201C"},
    {"&le;", "≤"},       {"&lfloor;", "⌊"},     {"&lowast;", "∗"},     {"&loz;", "◊"},      {"&lrm;", "\u200E"},
    {"&lsaquo;", "‹"},   {"&lsquo;", "\u2018"}, {"&lt;", "<"},         {"&macr;", "¯"},     {"&mdash;", "—"},
    {"&micro;", "µ"},    {"&minus;", "−"},      {"&mu;", "μ"},         {"&nabla;", "∇"},    {"&nbsp;", "\xC2\xA0"},
    {"&ndash;", "–"},    {"&ne;", "≠"},         {"&ni;", "∋"},         {"&not;", "¬"},      {"&notin;", "∉"},
    {"&nsub;", "⊄"},     {"&ntilde;", "ñ"},     {"&nu;", "ν"},         {"&oacute;", "ó"},   {"&ocirc;", "ô"},
    {"&oelig;", "œ"},    {"&ograve;", "ò"},     {"&oline;", "‾"},      {"&omega;", "ω"},    {"&omicron;", "ο"},
    {"&oplus;", "⊕"},    {"&or;", "∨"},         {"&ordf;", "ª"},       {"&ordm;", "º"},     {"&oslash;", "ø"},
    {"&otilde;", "õ"},   {"&otimes;", "⊗"},     {"&ouml;", "ö"},       {"&para;", "¶"},     {"&part;", "∂"},
    {"&permil;", "‰"},   {"&perp;", "⊥"},       {"&phi;", "φ"},        {"&pi;", "π"},       {"&piv;", "ϖ"},
    {"&plusmn;", "±"},   {"&pound;", "£"},      {"&prime;", "′"},      {"&prod;", "∏"},     {"&prop;", "∝"},
    {"&psi;", "ψ"},      {"&quot;", "\""},      {"&radic;", "√"},      {"&raquo;", "»"},    {"&rarr;", "→"},
    {"&rceil;", "⌉"},    {"&rdquo;", "\u201D"}, {"&reg;", "®"},        {"&rfloor;", "⌋"},   {"&rho;", "ρ"},
    {"&rlm;", "\u200F"}, {"&rsaquo;", "›"},     {"&rsquo;", "\u2019"}, {"&sbquo;", "‚"},    {"&scaron;", "š"},
    {"&sdot;", "⋅"},     {"&sect;", "§"},       {"&shy;", "\xC2\xAD"}, {"&sigma;", "σ"},    {"&sigmaf;", "ς"},
    {"&sim;", "∼"},      {"&spades;", "♠"},     {"&sub;", "⊂"},        {"&sube;", "⊆"},     {"&sum;", "∑"},
    {"&sup1;", "¹"},     {"&sup2;", "²"},       {"&sup3;", "³"},       {"&sup;", "⊃"},      {"&supe;", "⊇"},
    {"&szlig;", "ß"},    {"&tau;", "τ"},        {"&there4;", "∴"},     {"&theta;", "θ"},    {"&thetasym;", "ϑ"},
    {"&thinsp;", " "},   {"&thorn;", "þ"},      {"&tilde;", "˜"},      {"&times;", "×"},    {"&trade;", "™"},
    {"&uacute;", "ú"},   {"&uarr;", "↑"},       {"&ucirc;", "û"},      {"&ugrave;", "ù"},   {"&uml;", "¨"},
    {"&upsih;", "ϒ"},    {"&upsilon;", "υ"},    {"&uuml;", "ü"},       {"&xi;", "ξ"},       {"&yacute;", "ý"},
    {"&yen;", "¥"},      {"&yuml;", "ÿ"},       {"&zeta;", "ζ"},       {"&zwj;", "\u200D"}, {"&zwnj;", "\u200C"},
};

#define ENTITY_COUNT (sizeof(ENTITIES) / sizeof(ENTITIES[0]))

const char *html_entity_lookup(const char *entity, size_t len)
{
    if (!entity || len == 0) return NULL;

    size_t lo = 0, hi = ENTITY_COUNT;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const char *key = ENTITIES[mid].key;
        size_t key_len = strlen(key);
        int cmp = memcmp(entity, key, len < key_len ? len : key_len);
        if (cmp == 0 && len != key_len) cmp = len < key_len ? -1 : 1;
        if (cmp == 0) return ENTITIES[mid].value;
        if (cmp < 0) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return NULL;
}

size_t html_entity_count(void)
{
    return ENTITY_COUNT;
}

const char *html_entity_key(size_t i)
{
    return i < ENTITY_COUNT ? ENTITIES[i].key : NULL;
}
