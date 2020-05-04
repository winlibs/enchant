/* enchant
 * Copyright (C) 2003, 2004 Dom Lachowicz
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * In addition, as a special exception, Dom Lachowicz
 * gives permission to link the code of this program with
 * non-LGPL Spelling Provider libraries (eg: a MSFT Office
 * spell checker backend) and distribute linked combinations including
 * the two.  You must obey the GNU Lesser General Public License in all
 * respects for all of the code used other than said providers.  If you modify
 * this file, you may extend this exception to your version of the
 * file, but you are not obligated to do so.  If you do not wish to
 * do so, delete this exception statement from your version.
 */

/**
 *
 *  This file implements personal word list (PWL) dictionaries in the
 *  type EnchantPWL.
 *
 *  Under the hood, a PWL is stored as a Trie.  Checking strings for
 *  correctness and making suggestions is done by traversing the Trie
 *  while allowing a certain number of miss-steps.  Due to the prefix
 *  compression of the Trie, this allows all strings in the PWL within
 *  a given edit distance of the target word to be enumerated quite
 *  efficiently.
 *
 *  Ideas for the future:
 *
 *     - Order the suggestions first by edit distance, then by
 *       soundex key, to put the most similar sounding suggestions
 *       at the front of the list.  Would need a "soundex" that is
 *       general enough to handle languages other than English.
 *
 *     - iterative deepening to find suggestions, rather than a straight
 *       search to depth three.
 *
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <glib.h>
#include <glib/gstdio.h>
#include "enchant-provider.h"
#include "unused-parameter.h"

#include "pwl.h"

#define ENCHANT_PWL_MAX_ERRORS 3
#define ENCHANT_PWL_MAX_SUGGS 15

static const gunichar BOM = 0xfeff;

/*  A PWL dictionary is stored as a Trie-like data structure EnchantTrie.
 *  The EnchantTrie datatype is completely recursive - all child nodes
 *  are simply EnchantTrie pointers.  This means that all functions 
 *  that potentially modify a trie need to return the modified trie,
 *  as additional memory may have been allocated.
 *
 *  The empty trie is simply the null pointer.  If a trie contains
 *  a single string, it is recorded in the "value" attribute and
 *  "subtries" is set to NULL.  When two or more strings are contained,
 *  "value" is NULL and "subtries" is a GHashTable mapping the first
 *  character of each string to the subtrie containing the remainder of
 *  that string.
 *
 *  All strings stored in the Trie are assumed to be in UTF format.
 *  Branching is done on unicode characters, not individual bytes.
 */
typedef struct str_enchant_trie EnchantTrie;
struct str_enchant_trie
{
	char* value;           /* final string found under this node */
	GHashTable* subtries;  /* Maps characters to subtries */
};

struct str_enchant_pwl
{
	EnchantTrie* trie;
	char * filename;
	time_t file_changed;
	GHashTable *words_in_trie;
};

/* Special Trie node indicating the end of a string */
static EnchantTrie n_EOSTrie;
static EnchantTrie* EOSTrie = &n_EOSTrie;

/* mode for searching trie */
typedef enum enum_matcher_mode EnchantTrieMatcherMode;
enum enum_matcher_mode
{
	case_sensitive,
	case_insensitive
};

/*  The EnchantTrieMatcher structure maintains the state necessary to
 *  search for matching strings within an EnchantTrie.  It includes a
 *  callback function which will be called with each matching string
 *  as it is found.  The arguments to this function are:
 *
 *      - a freshly-allocated copy of the matching string, which must
 *        be freed by external code
 *      - the EnchantTrieMatcher object, giving the context of the match
 *        (e.g. number of errors)
 */
typedef struct str_enchant_trie_matcher EnchantTrieMatcher;
struct str_enchant_trie_matcher
{
	int num_errors;		/* Num errors encountered so far. */
	int max_errors;		/* Max errors before search should terminate */

	char* word;	/* Word being searched for */
	ssize_t word_pos;	/* Current position in the word */

	char* path;		    /* Path taken through the trie so far */
	ssize_t path_len;	/* Length of allocated path string */
	ssize_t path_pos;	/* Current end pos of path string */

	EnchantTrieMatcherMode mode;

	void (*cbfunc)(char*,EnchantTrieMatcher*); /* callback func */
	void* cbdata;		/* Private data for use by callback func */
};

/*  To allow the list of suggestions to be built up an item at a time,
 *  its state is maintained in an EnchantSuggList object.
 */
typedef struct str_enchant_sugg_list
{
	char** suggs;
	int* sugg_errs;
	size_t n_suggs;
} EnchantSuggList;

/*
 *   Function Prototypes
 */

static void enchant_pwl_add_to_trie(EnchantPWL *pwl,
					const char *const word, size_t len);
static void enchant_pwl_refresh_from_file(EnchantPWL* pwl);
static void enchant_pwl_check_cb(char* match,EnchantTrieMatcher* matcher);
static void enchant_pwl_suggest_cb(char* match,EnchantTrieMatcher* matcher);
static void enchant_trie_free(EnchantTrie* trie);
static void enchant_trie_free_cb(void*,void*,void*);
static EnchantTrie* enchant_trie_insert(EnchantTrie* trie,const char *const word);
static void enchant_trie_remove(EnchantTrie* trie,const char *const word);
static void enchant_trie_find_matches(EnchantTrie* trie,EnchantTrieMatcher *matcher);
static void enchant_trie_find_matches_cb(void* keyV,void* subtrieV,void* matcherV);
static EnchantTrieMatcher* enchant_trie_matcher_init(const char* const word, size_t len,
				int maxerrs,
				EnchantTrieMatcherMode mode,
				void(*cbfunc)(char*,EnchantTrieMatcher*),
				void* cbdata);
static void enchant_trie_matcher_free(EnchantTrieMatcher* matcher);
static void enchant_trie_matcher_pushpath(EnchantTrieMatcher* matcher,char* newchars);
static void enchant_trie_matcher_poppath(EnchantTrieMatcher* matcher,int num);

static int edit_dist(const char* word1, const char* word2);

#define enchant_lock_file(f)
#define enchant_unlock_file(f)

/**
 * enchant_pwl_init
 *
 * Returns: a new PWL object used to store/check/suggest words.
 */
EnchantPWL* enchant_pwl_init(void)
{
	EnchantPWL *pwl = g_new0(EnchantPWL, 1);
	pwl->words_in_trie = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

	return pwl;
}

/**
 * enchant_pwl_init_with_file
 *
 * Returns: a new PWL object used to store/check/suggest words
 * or NULL if the file cannot be opened or created
 */ 
EnchantPWL* enchant_pwl_init_with_file(const char * file)
{
	g_return_val_if_fail (file != NULL, NULL);

	FILE* fd = g_fopen(file, "a+");
	if(fd == NULL)
		return NULL;
	fclose(fd);
	EnchantPWL *pwl = enchant_pwl_init();
	pwl->filename = g_strdup(file);
	pwl->file_changed = 0;

	enchant_pwl_refresh_from_file(pwl);
	return pwl;
}

static void enchant_pwl_refresh_from_file(EnchantPWL* pwl)
{
	GStatBuf stats;
	if(!pwl->filename ||
	   g_stat(pwl->filename, &stats) != 0 || /* presumably I won't be able to open the file either */
	   pwl->file_changed == stats.st_mtime) /* nothing changed since last read */
		return;

	enchant_trie_free(pwl->trie);
	pwl->trie = NULL;
	g_hash_table_destroy (pwl->words_in_trie);
	pwl->words_in_trie = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

	FILE *f = g_fopen(pwl->filename, "r");
	if (!f) 
		return;

	pwl->file_changed = stats.st_mtime;

	enchant_lock_file (f);
	
	char buffer[BUFSIZ + 1];
	size_t line_number = 1;
	for (; NULL != (fgets (buffer, sizeof (buffer), f)); ++line_number)
		{
			char *line = buffer;
			if(line_number == 1 && BOM == g_utf8_get_char(line))
				line = g_utf8_next_char(line);

			if(line[strlen(line)-1] != '\n' && !feof(f)) /* ignore lines longer than BUFSIZ. */ 
				{
					g_warning ("Line too long (ignored) in %s at line:%zu\n", pwl->filename, line_number);
					while (NULL != (fgets (buffer, sizeof (buffer), f)))
						{
							if (line[strlen(buffer)-1]=='\n') 
								break;
						}
					continue;
				}

			g_strchomp(line);
			if( line[0] && line[0] != '#')
				{
					if(g_utf8_validate(line, -1, NULL))
						enchant_pwl_add_to_trie(pwl, line, strlen(line));
					else
						g_warning ("Bad UTF-8 sequence in %s at line:%zu\n", pwl->filename, line_number);
				}
		}
	
	enchant_unlock_file (f);
	fclose (f);
}

void enchant_pwl_free(EnchantPWL *pwl)
{
	enchant_trie_free(pwl->trie);
	g_free(pwl->filename);
	g_hash_table_destroy (pwl->words_in_trie);
	g_free(pwl);
}

static void enchant_pwl_add_to_trie(EnchantPWL *pwl,
					const char *const word, size_t len)
{
	char * normalized_word = g_utf8_normalize (word, len, G_NORMALIZE_NFD);
	if(NULL != g_hash_table_lookup (pwl->words_in_trie, normalized_word)) {
		g_free (normalized_word);
		return;
	}
	
	g_hash_table_insert (pwl->words_in_trie, normalized_word, g_strndup(word,len));

	pwl->trie = enchant_trie_insert(pwl->trie, normalized_word);
}

static void enchant_pwl_remove_from_trie(EnchantPWL *pwl,
					const char *const word, size_t len)
{
	char * normalized_word = g_utf8_normalize (word, len, G_NORMALIZE_NFD);

	if( g_hash_table_remove (pwl->words_in_trie, normalized_word) )
		{
			enchant_trie_remove(pwl->trie, normalized_word);
			if(pwl->trie && pwl->trie->subtries == NULL && pwl->trie->value == NULL) {
				enchant_trie_free (pwl->trie);
				pwl->trie = NULL; /* make trie empty if has no content */
			}
		}
	
	g_free(normalized_word);
}

void enchant_pwl_add(EnchantPWL *pwl,
			 const char *const word, size_t len)
{
	enchant_pwl_refresh_from_file(pwl);

	enchant_pwl_add_to_trie(pwl, word, len);

	if (pwl->filename != NULL)
	{
		FILE *f = g_fopen(pwl->filename, "a+");
		if (f)
			{
				/* Since this function does not signal I/O
				   errors, only use return values to avoid
				   doing things that seem futile. */

				enchant_lock_file (f);
				GStatBuf stats;
				if(g_stat(pwl->filename, &stats)==0)
					pwl->file_changed = stats.st_mtime;

				/* Add a newline if the file doesn't end with one. */
				if (fseek (f, -1, SEEK_END) == 0)
					{
						int c = getc (f);
						fseek (f, 0L, SEEK_CUR); /* ISO C requires positioning between read and write. */
						if (c != '\n')
							putc ('\n', f);
					}

				if (fwrite (word, sizeof(char), len, f) == len)
					{
						putc ('\n', f);
					}
				enchant_unlock_file (f);
				fclose (f);
			}	
	}
}

void enchant_pwl_remove(EnchantPWL *pwl,
			 const char *const word, size_t len)
{
	if(enchant_pwl_check(pwl, word, len) == 1)
		return;

	enchant_pwl_refresh_from_file(pwl);

	enchant_pwl_remove_from_trie(pwl, word, len);

	if (pwl->filename)
		{
			char * contents;
			size_t length;

			if(!g_file_get_contents(pwl->filename, &contents, &length, NULL))
				return;

			FILE *f = g_fopen(pwl->filename, "wb"); /*binary because g_file_get_contents reads binary*/
			if (f)
				{
					enchant_lock_file (f);
					char *key = g_strndup(word, len);

					char *filestart;
					if(BOM == g_utf8_get_char(contents))
						{
							filestart = g_utf8_next_char(contents);
							fwrite (contents, sizeof(char), filestart-contents, f);
						}
					else
						filestart = contents;

					char *searchstart = filestart;
					for(;;)
						{
							/*find word*/
							char *needle = strstr(searchstart, key);
							if(needle == NULL)
								{
									fwrite (searchstart, sizeof(char), length - (searchstart - contents), f);
									break;
								}
							else 
								{
									char* foundend = needle+len;
									if((needle == filestart || contents[needle-contents-1] == '\n' || contents[needle-contents-1] == '\r') &&
										(foundend == contents + length || *foundend == '\n' || *foundend == '\r'))
										{
											fwrite (searchstart, sizeof(char), needle - searchstart, f);
											searchstart = foundend;
											while (*searchstart == '\n' || *searchstart == '\r')
												++searchstart;
										}
									else {
										fwrite (searchstart, sizeof(char), needle - searchstart+1, f);
										searchstart = needle+1;
									}
								}
						}
					g_free(key);
					
					GStatBuf stats;
					if(g_stat(pwl->filename, &stats)==0)
						pwl->file_changed = stats.st_mtime;

					enchant_unlock_file (f);

					fclose (f);
				}	
			g_free(contents);
		}
}

static int enchant_pwl_contains(EnchantPWL *pwl, const char *const word, size_t len)
{
	int count = 0;
	EnchantTrieMatcher *matcher = enchant_trie_matcher_init(word, len, 0, case_sensitive, enchant_pwl_check_cb,
								&count);
	enchant_trie_find_matches(pwl->trie,matcher);
	enchant_trie_matcher_free(matcher);

	return count != 0;
}

static int enchant_is_all_caps(const char*const word, size_t len)
{
	g_return_val_if_fail (word && *word, 0);

	int hasCap = 0;
	for (const char *it = word; it < word + len; it = g_utf8_next_char(it))
		{
			GUnicodeType type = g_unichar_type(g_utf8_get_char(it));
			switch(type)
				{
				case G_UNICODE_UPPERCASE_LETTER:
					hasCap = 1;
					break;
				case G_UNICODE_TITLECASE_LETTER:
				case G_UNICODE_LOWERCASE_LETTER:
					return 0;

				case G_UNICODE_CONTROL:
				case G_UNICODE_FORMAT:
				case G_UNICODE_UNASSIGNED:
				case G_UNICODE_PRIVATE_USE:
				case G_UNICODE_SURROGATE:
				case G_UNICODE_MODIFIER_LETTER:
				case G_UNICODE_OTHER_LETTER:
				case G_UNICODE_COMBINING_MARK:
				case G_UNICODE_ENCLOSING_MARK:
				case G_UNICODE_NON_SPACING_MARK:
				case G_UNICODE_DECIMAL_NUMBER:
				case G_UNICODE_LETTER_NUMBER:
				case G_UNICODE_OTHER_NUMBER:
				case G_UNICODE_CONNECT_PUNCTUATION:
				case G_UNICODE_DASH_PUNCTUATION:
				case G_UNICODE_CLOSE_PUNCTUATION:
				case G_UNICODE_FINAL_PUNCTUATION:
				case G_UNICODE_INITIAL_PUNCTUATION:
				case G_UNICODE_OTHER_PUNCTUATION:
				case G_UNICODE_OPEN_PUNCTUATION:
				case G_UNICODE_CURRENCY_SYMBOL:
				case G_UNICODE_MODIFIER_SYMBOL:
				case G_UNICODE_MATH_SYMBOL:
				case G_UNICODE_OTHER_SYMBOL:
				case G_UNICODE_LINE_SEPARATOR:
				case G_UNICODE_PARAGRAPH_SEPARATOR:
				case G_UNICODE_SPACE_SEPARATOR:
				default:
					break;
				}
		}

	return hasCap;
}

static _GL_ATTRIBUTE_PURE int enchant_is_title_case(const char * const word, size_t len)
{
	g_return_val_if_fail (word && *word, 0);

	gunichar ch = g_utf8_get_char(word);
	GUnicodeType type = g_unichar_type(ch);
	if ((type != G_UNICODE_UPPERCASE_LETTER && type != G_UNICODE_TITLECASE_LETTER) ||
		ch != g_unichar_totitle(ch))
		return 0;
			
	for (const char* it = g_utf8_next_char(word); it < word + len; it = g_utf8_next_char(it))
		{
			type = g_unichar_type(g_utf8_get_char(it));
			if (type == G_UNICODE_UPPERCASE_LETTER || type == G_UNICODE_TITLECASE_LETTER)
				return 0;
		}

	return 1;
}

static gchar* enchant_utf8_strtitle(const gchar*str, gssize len)
{
	gchar *upperStr = g_utf8_strup(str, len); /* for locale-sensitive casing */
	gunichar title_case_char = g_unichar_totitle(g_utf8_get_char(upperStr));
	gchar title_case_utf8[7];
	gint utf8len = g_unichar_to_utf8(title_case_char, title_case_utf8);
	title_case_utf8[utf8len] = '\0';

	gchar *lowerTail = g_utf8_strdown(g_utf8_next_char(upperStr), -1);
	gchar *result = g_strconcat(title_case_utf8, lowerTail, NULL);
	g_free(upperStr);
	g_free(lowerTail);

	return result;
}

int enchant_pwl_check(EnchantPWL *pwl, const char *const word, size_t len)
{
	enchant_pwl_refresh_from_file(pwl);

	int exists = enchant_pwl_contains(pwl, word, len);
	
	if(exists)
		return 0;

	int isAllCaps = 0;
	if(enchant_is_title_case(word, len) || (isAllCaps = enchant_is_all_caps(word, len)))
		{
			char * lower_case_word = g_utf8_strdown(word, len);
			exists = enchant_pwl_contains(pwl, lower_case_word, strlen(lower_case_word));
			g_free(lower_case_word);
			if(exists)
				return 0;

			if(isAllCaps)
			{
				char * title_case_word = enchant_utf8_strtitle(word, len);
				exists = enchant_pwl_contains(pwl, title_case_word, strlen(title_case_word));
				g_free(title_case_word);
				if(exists)
					return 0;
			}
		}

	return 1; /* not found */
}

/* matcher callback when a match is found*/
static void enchant_pwl_check_cb(char* match,EnchantTrieMatcher* matcher)
{
	g_free(match);
	(*((int*)(matcher->cbdata)))++;
}

static void enchant_pwl_case_and_denormalize_suggestions(EnchantPWL *pwl, 
							 const char *const word, size_t len, 
							 EnchantSuggList* suggs_list)
{
	gchar* (*utf8_case_convert_function)(const gchar*str, gssize len) = NULL;
	if(enchant_is_title_case(word, len))
		utf8_case_convert_function = enchant_utf8_strtitle;
	else if (enchant_is_all_caps(word, len))
		utf8_case_convert_function = g_utf8_strup;
	
	for (size_t i = 0; i < suggs_list->n_suggs; ++i)
		{
			gchar* suggestion = g_hash_table_lookup (pwl->words_in_trie, suggs_list->suggs[i]);
			size_t suggestion_len = strlen(suggestion);

			gchar* cased_suggestion;
			if (utf8_case_convert_function && !enchant_is_all_caps(suggestion, suggestion_len))
				cased_suggestion = utf8_case_convert_function(suggestion, suggestion_len);
			else
				cased_suggestion = g_strndup(suggestion, suggestion_len);
			
			g_free(suggs_list->suggs[i]);
			suggs_list->suggs[i] = cased_suggestion;
		}
}

static int best_distance(char** suggs, const char *const word, size_t len)
{
	char *normalized_word = g_utf8_normalize (word, len, G_NORMALIZE_NFD);
	int best_dist = g_utf8_strlen(normalized_word, -1);

	for (char **sugg_it = suggs; *sugg_it; ++sugg_it)
		{
			char* normalized_sugg = g_utf8_normalize (*sugg_it, -1, G_NORMALIZE_NFD);
			int dist = edit_dist(normalized_word, normalized_sugg);
			g_free(normalized_sugg);
			best_dist = MIN (dist, best_dist);
		}

	g_free(normalized_word);
	return best_dist;
}

/* gives the best set of suggestions from pwl that are at least as good as the 
 * given suggs (if suggs == NULL just best from pwl) */
char** enchant_pwl_suggest(EnchantPWL *pwl, const char *const word,
			   size_t len, char** suggs, size_t* out_n_suggs)
{
	int max_dist = suggs ? best_distance(suggs, word, len) : ENCHANT_PWL_MAX_ERRORS;
	max_dist = MIN (max_dist, ENCHANT_PWL_MAX_ERRORS);

	enchant_pwl_refresh_from_file(pwl);

	EnchantSuggList sugg_list;
	sugg_list.suggs = g_new0(char*,ENCHANT_PWL_MAX_SUGGS+1);
	sugg_list.sugg_errs = g_new0(int,ENCHANT_PWL_MAX_SUGGS);
	sugg_list.n_suggs = 0;

	EnchantTrieMatcher *matcher = enchant_trie_matcher_init(word, len, max_dist,
								case_insensitive,
								enchant_pwl_suggest_cb,
								&sugg_list);
	enchant_trie_find_matches(pwl->trie,matcher);
	enchant_trie_matcher_free(matcher);

	g_free(sugg_list.sugg_errs);
	sugg_list.suggs[sugg_list.n_suggs] = NULL;
	(*out_n_suggs) = sugg_list.n_suggs;

	enchant_pwl_case_and_denormalize_suggestions(pwl, word, len, &sugg_list);
	
	return sugg_list.suggs;
}

/* matcher callback when a match is found*/
static void enchant_pwl_suggest_cb(char* match,EnchantTrieMatcher* matcher)
{
	EnchantSuggList *sugg_list = (EnchantSuggList*)(matcher->cbdata);

	/* only get best errors so adapt */
	if(matcher->num_errors < matcher->max_errors)
		matcher->max_errors = matcher->num_errors;

	/* Find appropriate location in the array, if any */
	/* In future, this could be done using binary search...  */
	size_t loc;
	for(loc=0; loc < sugg_list->n_suggs; loc++) {
		/* Better than an existing suggestion, so stop */
		if(sugg_list->sugg_errs[loc] > matcher->num_errors) {
			break;
		}
		/* Already in the list with better score, just return */
		if(strcmp(match,sugg_list->suggs[loc])==0) {
			g_free(match);
			return;
		}
	}
	/* If it's not going to fit, just throw it away */
	if(loc >= ENCHANT_PWL_MAX_SUGGS) {
		g_free(match);
		return;
	}

	int changes = 1;  /* num words added to list */
	
	/* Remove all elements with worse score */
	for(size_t i=loc; i < sugg_list->n_suggs; i++){
		g_free(sugg_list->suggs[i]);
		changes--;
	}

	sugg_list->suggs[loc] = match;
	sugg_list->sugg_errs[loc] = matcher->num_errors;
	sugg_list->n_suggs = sugg_list->n_suggs + changes;
}

static void enchant_trie_free(EnchantTrie* trie)
{
	/* Don't try to free NULL or the EOSTrie pointer */
	if(trie == NULL || trie == EOSTrie) {
		return;
	}

	/* Because we have not set a destroy function for the hashtable
	 * (to make code cleaner below), we need to explicitly free all
	 * subtries with a callback function.
	 */
	if (trie->subtries != NULL) {
		g_hash_table_foreach(trie->subtries,enchant_trie_free_cb,NULL);
		g_hash_table_destroy(trie->subtries);
	}

	g_free(trie->value);
	g_free(trie);
}

static void enchant_trie_free_cb(void* key _GL_UNUSED_PARAMETER,
				 void* value,
				 void* data _GL_UNUSED_PARAMETER)
{
	enchant_trie_free((EnchantTrie*) value);
}

static EnchantTrie* enchant_trie_insert(EnchantTrie* trie,const char *const word)
{
	if (trie == NULL) {
		trie = g_new0(EnchantTrie, 1);
	}

	if (trie->value == NULL) {
		if (trie->subtries == NULL) {
			/*  When single word, store in trie->value */
			trie->value = g_strdup(word);
		} else {
			/* Store multiple words in subtries */
			if (word[0] == '\0') {
				/* Mark end-of-string with special node */
				char *tmpWord = g_strdup("");
				g_hash_table_insert(trie->subtries, tmpWord, EOSTrie);
			} else {
				ssize_t nxtCh = (ssize_t)(g_utf8_next_char(word)-word);
				char *tmpWord = g_strndup(word,nxtCh);
				EnchantTrie* subtrie = g_hash_table_lookup(trie->subtries, tmpWord);
				subtrie = enchant_trie_insert(subtrie, word + nxtCh);
				g_hash_table_insert(trie->subtries, tmpWord, subtrie);
			}
		}
	} else {
		/* Create new hash table for subtries, and reinsert */
		trie->subtries = g_hash_table_new_full(g_str_hash, 
						       g_str_equal, g_free, NULL);
		char *tmpWord = trie->value;
		trie->value = NULL;
		enchant_trie_insert(trie, tmpWord);
		enchant_trie_insert(trie, word);
		g_free(tmpWord);
	}

	return trie;
}

static void enchant_trie_remove(EnchantTrie* trie,const char *const word)
{
	if (trie == NULL)
		return;

	if (trie->value == NULL) {
		if (trie->subtries != NULL) {
			/* Store multiple words in subtries */
			if (word[0] == '\0') {
				/* End-of-string is marked with special node */
				g_hash_table_remove(trie->subtries, "");
			} else {
				ssize_t nxtCh = (ssize_t)(g_utf8_next_char(word) - word);
				char *tmpWord = g_strndup(word, nxtCh);
				EnchantTrie *subtrie = g_hash_table_lookup(trie->subtries, tmpWord);
				enchant_trie_remove(subtrie, word + nxtCh);

				if(subtrie->subtries == NULL && subtrie->value == NULL) {
					g_hash_table_remove(trie->subtries, tmpWord);
					enchant_trie_free (subtrie);
				}

				g_free(tmpWord);
			}

			if(g_hash_table_size(trie->subtries) == 1)
				{
					GList* keys = g_hash_table_get_keys(trie->subtries);
					char *key = (char*) keys->data;
					EnchantTrie *subtrie = g_hash_table_lookup(trie->subtries, key);

					/* only remove trie nodes that have values by propagating these up */
					if(subtrie->value)
						{
							trie->value = g_strconcat(key, subtrie->value, NULL);
							enchant_trie_free(subtrie);
							g_hash_table_destroy(trie->subtries);
							trie->subtries = NULL;
						}

					g_list_free(keys);
				}
		}
	} else {
		if(strcmp(trie->value, word) == 0)
		{
			g_free(trie->value);
			trie->value = NULL;
		}
	}
}

static EnchantTrie* enchant_trie_get_subtrie(EnchantTrie* trie, 
					     EnchantTrieMatcher* matcher,
					     char** nxtChS)
{
	if(trie->subtries == NULL || *nxtChS == NULL)
		return NULL;

	EnchantTrie *subtrie = g_hash_table_lookup(trie->subtries,*nxtChS);
	if(subtrie == NULL && matcher->mode == case_insensitive) {
		char* nxtChSUp = g_utf8_strup(*nxtChS, -1); /* we ignore the title case scenario since that will give us an edit_distance of one which is acceptable since this mode is used for suggestions*/
		g_free(*nxtChS);
		*nxtChS = nxtChSUp;
		subtrie = g_hash_table_lookup(trie->subtries,nxtChSUp);
	}
	return subtrie;
}

static void enchant_trie_find_matches(EnchantTrie* trie,EnchantTrieMatcher *matcher)
{
	g_return_if_fail(matcher);

	/* Can't match in the empty trie */
	if(trie == NULL) {
		return;
	}

	/* Bail out if over the error limits */
	if(matcher->num_errors > matcher->max_errors){
		return;
	}

	/* If the end of a string has been reached, no point recursing */
	if (trie == EOSTrie) {
		size_t word_len = strlen(matcher->word);
		int errs = matcher->num_errors;
		if((ssize_t)word_len > matcher->word_pos) {
			matcher->num_errors = errs + word_len - matcher->word_pos;
		}
		if (matcher->num_errors <= matcher->max_errors) {
			matcher->cbfunc(g_strdup(matcher->path),matcher);
		}
		matcher->num_errors = errs;
		return;
	}

	/* If there is a value, just check it, no recursion */
	if (trie->value != NULL) {
		gchar* value;
		int errs = matcher->num_errors;
		value = trie->value;
		if(matcher->mode == case_insensitive)
			{
				value = g_utf8_strdown(value, -1);
			}
		matcher->num_errors = errs + edit_dist(value, 
					   &(matcher->word[matcher->word_pos]));
		if(matcher->mode == case_insensitive)
			{
				g_free(value);
			}

		if (matcher->num_errors <= matcher->max_errors) {
			matcher->cbfunc(g_strconcat(matcher->path,
							trie->value,NULL),
					matcher);
		}
		matcher->num_errors = errs;
		return;
	}

	ssize_t nxtChI = (ssize_t)(g_utf8_next_char(&matcher->word[matcher->word_pos]) - matcher->word);
	char *nxtChS = g_strndup(&matcher->word[matcher->word_pos],
				 (nxtChI - matcher->word_pos));

	/* Precisely match the first character, and recurse */
	EnchantTrie* subtrie = enchant_trie_get_subtrie(trie, matcher, &nxtChS);
	if (subtrie != NULL) {
		enchant_trie_matcher_pushpath(matcher,nxtChS);
		ssize_t oldPos = matcher->word_pos;
		matcher->word_pos = nxtChI;
		enchant_trie_find_matches(subtrie,matcher);
		matcher->word_pos = oldPos;
		enchant_trie_matcher_poppath(matcher,strlen(nxtChS));
	}

	g_free(nxtChS);

	matcher->num_errors++;
	if (matcher->word[matcher->word_pos] != '\0') {
		/* Match on inserting word[0] */
		ssize_t oldPos = matcher->word_pos;
		matcher->word_pos = nxtChI;
		enchant_trie_find_matches(trie,matcher);
		matcher->word_pos = oldPos;
	}
	/* for each subtrie, match on delete or substitute word[0] or transpose word[0] and word[1] */
	g_hash_table_foreach(trie->subtries,
				enchant_trie_find_matches_cb,
				matcher);
	matcher->num_errors--;
}

static void enchant_trie_find_matches_cb(void* keyV,void* subtrieV,void* matcherV)
{
	char *key = (char*) keyV;
	EnchantTrie *subtrie = (EnchantTrie*) subtrieV;
	EnchantTrieMatcher *matcher = (EnchantTrieMatcher*) matcherV;

	ssize_t nxtChI = (ssize_t) (g_utf8_next_char(&matcher->word[matcher->word_pos]) - matcher->word);

	/* Dont handle actual matches, that's already done */
	if (strncmp(key,&matcher->word[matcher->word_pos],nxtChI-matcher->word_pos) == 0) {
		return;
	}

	enchant_trie_matcher_pushpath(matcher,key);

	/* Match on deleting word[0] */
	enchant_trie_find_matches(subtrie,matcher);
	/* Match on substituting word[0] */
	ssize_t oldPos = matcher->word_pos;
	matcher->word_pos = nxtChI;
	enchant_trie_find_matches(subtrie,matcher);

	enchant_trie_matcher_poppath(matcher,strlen(key));

	/* Match on transposing word[0] and word[1] */
	char *key2 = g_strndup(&matcher->word[oldPos],nxtChI-oldPos);
	EnchantTrie *subtrie2 = enchant_trie_get_subtrie(subtrie, matcher, &key2);

	if(subtrie2 != NULL) {
		nxtChI = (ssize_t) (g_utf8_next_char(&matcher->word[matcher->word_pos]) - matcher->word);
		if (strncmp(key,&matcher->word[matcher->word_pos],nxtChI-matcher->word_pos) == 0) {
			matcher->word_pos = nxtChI;
			enchant_trie_matcher_pushpath(matcher,key);
			enchant_trie_matcher_pushpath(matcher,key2);

			enchant_trie_find_matches(subtrie2,matcher);
			enchant_trie_matcher_poppath(matcher,strlen(key2));
			enchant_trie_matcher_poppath(matcher,strlen(key));
		}
	}

	g_free(key2);
	
	matcher->word_pos = oldPos;
}

static EnchantTrieMatcher* enchant_trie_matcher_init(const char* const word,
						     size_t len,
						     int maxerrs,
						     EnchantTrieMatcherMode mode,
						     void(*cbfunc)(char*,EnchantTrieMatcher*),
						     void* cbdata)
{
	char * normalized_word = g_utf8_normalize (word, len, G_NORMALIZE_NFD);
	len = strlen(normalized_word);

	char * pattern = normalized_word;
	if (mode == case_insensitive)
		{
			pattern = g_utf8_strdown (normalized_word, len);
			g_free(normalized_word);
		}

	EnchantTrieMatcher* matcher = g_new(EnchantTrieMatcher,1);
	matcher->num_errors = 0;
	matcher->max_errors = maxerrs;
	matcher->word = g_new0(char,len+maxerrs+2); // Ensure matcher does not overrun buffer: +2 for transpose check
	strcpy(matcher->word, pattern);
	g_free(pattern);
	matcher->word_pos = 0;
	matcher->path = g_new0(char,len+maxerrs+1);
	matcher->path[0] = '\0';
	matcher->path_len = len+maxerrs+1;
	matcher->path_pos = 0;
	matcher->mode = mode;
	matcher->cbfunc = cbfunc;
	matcher->cbdata = cbdata;

	return matcher;
}

static void enchant_trie_matcher_free(EnchantTrieMatcher* matcher)
{
	g_free(matcher->word);
	g_free(matcher->path);
	g_free(matcher);
}

static void enchant_trie_matcher_pushpath(EnchantTrieMatcher* matcher,char* newchars)
{
	ssize_t len = strlen(newchars);
	if(matcher->path_pos + len >= matcher->path_len) {
		matcher->path_len = matcher->path_len + len + 10;
		matcher->path = g_renew(char,matcher->path,matcher->path_len);
	}

	for (ssize_t i = 0; i < len; i++) {
		matcher->path[matcher->path_pos + i] = newchars[i];
	}
	matcher->path_pos = matcher->path_pos + len;
	matcher->path[matcher->path_pos] = '\0';
}

static void enchant_trie_matcher_poppath(EnchantTrieMatcher* matcher,int num)
{
	g_return_if_fail(matcher->path_pos >= 0);
	matcher->path_pos = matcher->path_pos - num;
	if(matcher->path_pos < 0) {
		matcher->path_pos = 0;
	}
	matcher->path[matcher->path_pos] = '\0';
}

static int edit_dist(const char* utf8word1, const char* utf8word2)
{
	glong len1, len2;
	gunichar * word1 = g_utf8_to_ucs4_fast(utf8word1, -1, &len1);
	gunichar * word2 = g_utf8_to_ucs4_fast(utf8word2, -1, &len2);

	int * table = g_new0(int, (len1+1)*(len2+1));
	
	/* Initialise outer rows of table */
	for (glong i = 0; i < len1 + 1; i++)
		table[i*(len2+1)] = i;
	for (glong i = 0; i < len2 + 1; i++)
		table[i] = i;

	/* Fill in table in dynamic programming style */
	for (glong i = 1; i < len1+1; i++){
		for (glong j = 1; j < len2+1; j++) {
			int cost = word1[i-1] != word2[j-1];
			int v1 = table[(i-1)*(len2+1)+j] + 1;
			if (i > 1 && j > 1 && word1[i-1] == word2[j-2] && word1[i-2] == word2[j-1]) {
				v1 = MIN (v1, table[(i-2)*(len2+1)+(j-2)] + cost);
			}

			int v2 = table[i*(len2+1)+(j-1)] + 1;
			int v3 = table[(i-1)*(len2+1)+(j-1)] + cost;
			table[i*(len2+1)+j] = MIN (MIN (v1, v2), v3);
		}
	}

	int cost = table[len1*(len2+1) + len2];
	g_free(word1);
	g_free(word2);
	g_free(table);
	return cost;
}
