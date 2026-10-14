// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* The phonebook (phonebook.rs): the contacts in every file named
 * contacts.vcf, in any case, anywhere on the SD card down to
 * PB_SEARCH_DEPTH folders deep, as a phone or a mail program exports them
 * (vCard 2.1, 3.0 or 4.0). A contact in two files is listed once. They are
 * kept sorted by name, ignoring case and accents.
 *
 * The card is searched once, the first time anything asks for a contact or
 * a name, and again on rs_phonebook_rescan(); between searches the files
 * found are read again whenever one of them changes. Reading is done on a
 * thread of its own: until the first read ends the book is empty and
 * rs_phonebook_loading() is true.
 *
 * Stars are this device's own, kept by name in /sdcard/phone/starred.json:
 * a .vcf is never written to. */

#define PB_NAME_MAX      64   /* UTF-8, NUL included */
#define PB_NUMBER_MAX    33
#define PB_LABEL_MAX     12
#define PB_NUMBERS_MAX   6    /* a contact's numbers beyond these are dropped */
#define PB_CONTACTS_MAX  2000
#define PB_SEARCH_DEPTH  4

typedef struct {
    char number[PB_NUMBER_MAX];  /* as the card writes it: "+49 176 1234567" */
    char label[PB_LABEL_MAX];    /* "Mobile", "Home", "Work", "Main", "Other" */
} pb_number_t;

typedef struct {
    char        name[PB_NAME_MAX];    /* never "": a contact with no name is
                                         named by its organisation or first
                                         number */
    bool        starred;
    uint8_t     number_count;         /* 0 for a contact with no number */
    pb_number_t numbers[PB_NUMBERS_MAX];
} pb_contact_t;

/* Search the card again for contacts.vcf files, and read them; returns at
 * once. ESP_ERR_INVALID_STATE while a read is under way. */
esp_err_t rs_phonebook_rescan(void);

/* Whether the card is being searched or read. */
bool rs_phonebook_loading(void);

/* Changes whenever the contacts or their stars do, so a list is rebuilt only
 * when needed. Asking also starts the first search, if none has run. */
uint32_t rs_phonebook_seq(void);

/* How many contacts.vcf files the last search found, and the first of them,
 * for an app to say where its contacts come from. */
uint32_t rs_phonebook_file_count(void);
bool rs_phonebook_first_file(char *path, size_t size);

uint32_t rs_phonebook_count(void);

/* The contact at `index`, in name order. */
bool rs_phonebook_get(uint32_t index, pb_contact_t *out);

/* The contacts whose name holds `query`, ignoring case and accents, or with
 * a number holding its digits when it has at least two; in name order. Up
 * to `max` indices go into `indices`; the return is how many matched in all.
 * An empty or NULL query matches every contact. */
uint32_t rs_phonebook_search(const char *query, uint32_t *indices, uint32_t max);

/* The starred contacts, the same way. */
uint32_t rs_phonebook_starred(uint32_t *indices, uint32_t max);

esp_err_t rs_phonebook_set_starred(uint32_t index, bool starred);

/* The contact with `number`, matched by its last 9 digits (all of them for a
 * shorter one), so "+4917612345678" finds "0176 12345678". -1 for none. */
int32_t rs_phonebook_find_number(const char *number);

/* That contact's name, into `name`; false, leaving it untouched, for none. */
bool rs_phonebook_name_for(const char *number, char *name, size_t size);
