/*
 * check_spice.cpp - checker for a Spice netlist
 *
 * Copyright (C) 2004 Stefan Jahn <stefan@lkcc.org>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this package; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.  
 *
 * $Id: check_spice.cpp,v 1.2 2004/10/31 12:35:46 ela Exp $
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <float.h>

#ifdef __MINGW32__
#define strcasecmp stricmp
#endif

#include "constants.h"
#include "check_spice.h"

/* Global definitions for parser and checker. */
struct definition_t * definition_root = NULL;
struct definition_t * subcircuit_root = NULL;
struct value_t * value_root = NULL;

// List of available Spice components.
struct define_t spice_definition_available[] =
{
  /* resistor */
  { "R", 2, PROP_COMPONENT, PROP_NO_SUBSTRATE, PROP_LINEAR, 
    { { "R", PROP_REAL, { 50, PROP_NO_STR }, PROP_NO_RANGE }, PROP_NO_PROP },
    { { "Temp", PROP_REAL, { 26.85, PROP_NO_STR }, { K, PROP_VAL_MAX } },
      PROP_NO_PROP }
  },
  /* inductor */
  { "L", 2, PROP_COMPONENT, PROP_NO_SUBSTRATE, PROP_LINEAR,
    { { "L", PROP_REAL, { 1e-9, PROP_NO_STR }, PROP_NO_RANGE }, PROP_NO_PROP },
    { PROP_NO_PROP }
  },
  /* capacitor */
  { "C", 2, PROP_COMPONENT, PROP_NO_SUBSTRATE, PROP_LINEAR,
    { { "C", PROP_REAL, { 1e-12, PROP_NO_STR }, PROP_NO_RANGE },
      PROP_NO_PROP },
    { PROP_NO_PROP }
  },
  { "V", 2, PROP_COMPONENT, PROP_NO_SUBSTRATE, PROP_LINEAR,
    { { "U", PROP_REAL, { 1e-12, PROP_NO_STR }, PROP_NO_RANGE },
      PROP_NO_PROP },
    { PROP_NO_PROP }
  },
  { "Q", 3, PROP_COMPONENT, PROP_NO_SUBSTRATE, PROP_LINEAR,
    { PROP_NO_PROP },
    { PROP_NO_PROP }
  },

  /* end of list */
  { NULL, 0, 0, 0, 0, { PROP_NO_PROP }, { PROP_NO_PROP } }
};

// Include also the Qucs definitions.
#include "qucsdefs.h"

// Short definition for iterating a list of values.
#define foreach_value(values,val) \
  for ((val) = (values); (val) != NULL; (val) = (val)->next) \
    if (!((val)->hint & HINT_DONE))

/* The function reverses the order of the given value list and returns
   the reversed list. */
struct value_t * netlist_reverse_values (struct value_t * values) {
  struct value_t * root, * next;
  for (root = NULL; values != NULL; values = next) {
    next = values->next;
    values->next = root;
    root = values;
  }
  return root;
}

/* The function reverses the order of the given pair list and returns
   the reversed list. */
struct pair_t * netlist_reverse_pairs (struct pair_t * pairs) {
  struct pair_t * root, * next;
  for (root = NULL; pairs != NULL; pairs = next) {
    next = pairs->next;
    pairs->next = root;
    root = pairs;
  }
  return root;
}

/* The function reverses the order of the given node list and returns
   the reversed list. */
struct node_t * netlist_reverse_nodes (struct node_t * nodes) {
  struct node_t * root, * next;
  for (root = NULL; nodes != NULL; nodes = next) {
    next = nodes->next;
    nodes->next = root;
    root = nodes;
  }
  return root;
}

// The function appends lists of pairs and returns the resulting list.
struct pair_t *
netlist_append_pairs (struct pair_t * p1, struct pair_t * p2) {
  if (p1 == NULL) return p2;
  struct pair_t * pair;
  for (pair = p1; pair->next != NULL; pair = pair->next);
  pair->next = p2;
  return p1;
}

// The function appends lists of values and returns the resulting list.
struct value_t *
netlist_append_values (struct value_t * v1, struct value_t * v2) {
  if (v1 == NULL) return v2;
  struct value_t * val;
  for (val = v1; val->next != NULL; val = val->next);
  val->next = v2;
  return v1;
}

// The function appends lists of nodes and returns the resulting list.
struct node_t *
netlist_append_nodes (struct node_t * n1, struct node_t * n2) {
  if (n1 == NULL) return n2;
  struct node_t * node;
  for (node = n1; node->next != NULL; node = node->next);
  node->next = n2;
  return n1;
}

/* This function goes through the list of available definitions and
   checks whether the given component type can be found. */
static struct define_t * spice_find_definition (char * n) {
  struct define_t * def;
  for (def = spice_definition_available; def->type != NULL; def++) {
    if (!strcasecmp (n, def->type)) return def;
  }
  return NULL;
}

/* The function creates a single translated spice node. */
static struct node_t * spice_translate_node (char * node) {
  struct node_t * n = create_node ();
  if (!strcmp (node, "0")) { // translate ground node
    n->node = strdup ("gnd");
  }
  else {                     // translated other node than ground
    n->node = (char *) malloc (5 + strlen (node));
    strcpy (n->node, "_net");
    strcat (n->node, node);
  }
  return n;
}

/* The function creates the nodes list for the given component. */
static struct node_t * spice_get_nodes (struct definition_t * def) {

  // has this component actually any nodes to define?
  int nodes = def->define->nodes;
  if (nodes <= 0) return NULL;

  // go through value list and try identifying nodes
  struct value_t * val;
  struct node_t * root = NULL;
  int i = 0;
  foreach_value (def->values, val) {
    if (val->hint & (HINT_NODE | HINT_NUMBER)) {
      struct node_t * n = spice_translate_node (val->ident);
      n->next = root;
      root = n;
      val->hint |= HINT_DONE;
      if (++i >= nodes) break;
    }
  }
  // return reversed list of nodes
  return netlist_reverse_nodes (root);
}

/* Evaluates the unit scale in a property value.  It adjusts the
   actual value and omits the scale.  The function returns NULL if
   there is no valid scale found and otherwise the corrected scale.
   The given endptr is set to the character after the last valid scale
   character. */
static char * spice_evaluate_scale (char * value, char **endptr,
				    double * factor) {
  char * scale = NULL;
  *factor = 1.0;
  switch (*value) {
  case 'T': case 't':
    value++; scale = "T"; break;
  case 'G': case 'g':
    value++; scale = "G"; break;
  case 'M':
    value++; scale = "M";
    if ((value[1] == 'e' || value[1] == 'E') &&
	(value[2] == 'g' || value[2] == 'G')) {
      value += 2;
    }
    break;
  case 'k': case 'K':
    value++; scale = "k"; break;
  case 'm':
    value++; scale = "m";
    if ((value[1] == 'i' || value[1] == 'I') &&
	(value[2] == 'l' || value[2] == 'L')) {
      value += 2; *factor = 2.54e-5;
    }
    if ((value[1] == 'e' || value[1] == 'E') &&
	(value[2] == 'g' || value[2] == 'G')) {
      value += 2; scale = "M";
    }
    break;
  case 'u': case 'U':
    value++; scale = "u"; break;
  case 'n': case 'N':
    value++; scale = "n"; break;
  case 'p': case 'P':
    value++; scale = "p"; break;
  case 'f': case 'F':
    value++; scale = "f"; break;
  }
  *endptr = value;
  return scale;
}

/* This little function takes the given string value, converts it into
   an appropriate real value and save optional scale and unit. */
static struct value_t * spice_create_value (char * ident) {
  struct value_t * val;
  char * end, * str;
  double value = 1.0;
  val = create_value ();
  val->value = strtod (ident, &end);
  if (*end) {
    str = spice_evaluate_scale (end, &end, &value);
    val->value *= value;
    val->scale = str ? strdup (str) : NULL;
    if (*end) val->unit = strdup (end);
  }
  return val;
}

/* The function identifies key/value pairs in the value list of the
   given definition and saves these into an appropriate list. */
static struct pair_t * spice_get_pairs (struct definition_t * def) {
  struct value_t * val;
  struct pair_t * root = NULL;
  struct property_t * prop;
  int i = 0;
  foreach_value (def->values, val) {
    // if there is a value given and no description, it is required
    if (val->hint & HINT_NUMBER) {
      prop = &def->define->required[i++];
      struct pair_t * p = create_pair ();
      p->key = strdup (prop->key);
      p->value = spice_create_value (val->ident);
      p->next = root;
      root = p;
      val->hint |= HINT_DONE;
    }
    else break;
  }
  return root;
}

/* The function goes through the list of definitions and tries to find
   the given device model.  It returns NULL if there is no such
   model. */
static struct definition_t * spice_find_device (char * type) {
  struct definition_t * def;
  for (def = definition_root; def != NULL; def = def->next) {
    if (def->action && !strcasecmp (def->type, "MODEL") &&
	!strcasecmp (def->instance, type)) {
      return def;
    }
  }
  return NULL;
}

/* Looks for the first possible .MODEL instance name in the given list. */
static struct value_t *
spice_find_device_instance (struct definition_t * def) {
  struct value_t * val;
  foreach_value (def->values, val) {
    if (val->hint & HINT_NAME) return val;
  }
  return NULL;
}

// Little helper structure for device translations.
struct spice_device_t {
  char * type;            // Spice type
  char * trans_type;      // Qucs type
  char * trans_type_prop; // value of 'Type' in Qucs
}
spice_devices[] = {
  { "NPN",  "BJT",    "npn"  },
  { "PNP",  "BJT",    "pnp"  },
  { "NJF",  "JFET",   "nfet" },
  { "PJF",  "JFET",   "pfet" },
  { "NMOS", "MOSFET", "nfet" },
  { "PMOS", "MOSFET", "pfet" },
  { "D",    "Diode",  NULL   },
  { NULL, NULL, NULL }
};

/* The function goes through the given value list and creates an
   appropriate list of key/value pairs stored inside .MODEL
   specifications. */
static struct pair_t * spice_generate_Model_pairs (struct value_t * values) {
  struct pair_t * root = NULL;
  struct value_t * val;
  foreach_value (values, val) {
    if (val->hint & HINT_PAIR) {
      struct pair_t * p = create_pair ();
      p->key = strdup (val->ident);
      p->value = spice_create_value (val->unit);
      p->next = root;
      root = p;
      if (val->hint & HINT_MSTOP) break;
    }
    else break;
  }
  return netlist_reverse_pairs (root);
}

/* This function adjusts the given device instance definition using
   the the given device definition. */
static void spice_adjust_device (struct definition_t * def,
				 struct definition_t * trandef) {

  // first find the starting point of the definition
  struct value_t * val, * start = NULL;
  foreach_value (trandef->values, val) {
    if (val->hint & HINT_MSTART) { start = val; break; }
  }
  if (start == NULL) {
    foreach_value (trandef->values, val) {
      if (val->hint & HINT_NAME) { start = val; break; }
    }
  }

  // second: look for an appropriate available translation
  struct spice_device_t * tran;
  for (tran = spice_devices; tran->type; tran++) {
    if (!strcasecmp (tran->type, start->ident)) {
      struct pair_t * p;
      // append properties
      p = spice_generate_Model_pairs (start->next);
      def->pairs = netlist_append_pairs (def->pairs, p);
      // adjust type of device
      free (def->type);
      def->type = strdup (tran->trans_type);
      // append "Type" property
      if (tran->trans_type_prop != NULL) {
	p = create_pair ();
	p->key = strdup ("Type");
	p->value = create_value ();
	p->value->ident = strdup (tran->trans_type_prop);
	def->pairs = netlist_append_pairs (p, def->pairs);
      }
      break;
    }
  }
}

/* Translate .MODEL specifications in Q instances. */
static void spice_translate_device (struct definition_t * def) {
  struct value_t * inst = spice_find_device_instance (def);
  struct definition_t * tran = spice_find_device (inst->ident);
  if (tran) {
    inst->hint |= HINT_DONE;
    spice_adjust_device (def, tran);
  }
}

/* Goes through the list of available definition and returns the
   appropriate component definition. */
static struct define_t *
spice_get_qucs_definition (struct definition_t * def) {
  struct define_t * entry;
  for (entry = qucs_definition_available; entry->type != NULL; entry++) {
    if (!strcmp (entry->type, def->type))
      return entry;
  }
  return NULL;
}

// Helper structure for node translations.
struct node_translation_t {
  char * type;    // the type of definition
  int Mapping[6]; // node ordering
  int Default[6]; // default nodes
}
node_translation[] = {
  { "BJT",
    { 2, 1, 3, 4, -1 },
    { 1, 2, 3, 0, -1 }
  },
  { NULL, { -1 }, { -1 } }
};

// Counts the number of nodes stored in the given definition.
static int spice_count_nodes (struct definition_t * def) {
  int res = 0;
  for (struct node_t * node = def->nodes; node != NULL; node = node->next)
    res++;
  return res;
}

// Deletes node list of a definition.
static void spice_free_nodes (struct node_t * node) {
  struct node_t * n;
  for (; node != NULL; node = n) {
    n = node->next;
    free (node->node);
    free (node);
  }
}

// The function returns the node specified at the given position.
static struct node_t * spice_get_node (struct definition_t * def, int pos) {
  struct node_t * node;
  int i;
  for (i = 1, node = def->nodes; node != NULL; node = node->next, i++)
    if (i == pos) return node;
  return NULL;
}

/* This function transformes the collected node information in the
   Spice list into the appropriate Qucs definition. */
static void spice_translate_nodes (struct definition_t * def) {
  struct node_translation_t * nodes;
  struct node_t * node;
  // find node translator
  for (nodes = node_translation; nodes->type != NULL; nodes++) {
    if (!strcmp (nodes->type, def->type)) break;
  }
  if (nodes->type == NULL) return;

  struct define_t * entry = spice_get_qucs_definition (def);
  // adjust the number of nodes and append default nodes
  if (entry->nodes > spice_count_nodes (def)) {
    for (int n = spice_count_nodes (def); n < entry->nodes; n++) {
      node = create_node ();
      if (nodes->Default[n] == 0) { // default is the ground node
	node->node = strdup ("gnd");
      }
      else {                       // default is some other node
	struct node_t * t = spice_get_node (def, nodes->Default[n]);
	node->node = strdup (t->node);
      }
      def->nodes = netlist_append_nodes (def->nodes, node);
    }
  }
  // remap the node definitions if necessary
  struct node_t * root = NULL;
  for (int n = entry->nodes - 1; n >= 0; n--) {
    struct node_t * t = spice_get_node (def, nodes->Mapping[n]);
    node = create_node ();
    node->node = strdup (t->node);
    node->next = root;
    root = node;
  }
  spice_free_nodes (def->nodes);
  def->nodes = root;
}

// Helper structure for unit translations.
struct unit_translation_t {
  char * key;
  char * trans;
}
unit_translation[] = {
  { "OHM",  "Ohm" },
  { "MHO",  "S" },
  { "S",    "s" },
  { "H",    "H" },
  { "F",    "F" },
  { "HZ",   "Hz" },
  { "V",    "V" },
  { "Volt", "V" },
  { "A",    "A" },
  { "M",    "m" },
  { NULL, NULL }
};

/* This function tries to figure out units given in the Spice list and
   translates these patterns into valid Qucs units. */
static void spice_translate_units (struct definition_t * def) {
  for (struct pair_t * pair = def->pairs; pair != NULL; pair = pair->next) {
    struct value_t * value = pair->value;
    if (value->unit) {
      struct unit_translation_t * unit;
      int found = 0;
      for (unit = unit_translation; unit->key != NULL; unit++) {
	if (!strcasecmp (unit->key, value->unit)) {
	  free (value->unit);
	  value->unit = strdup (unit->trans);
	  found++;
	  break;
	}
      }
      if (!found) { // no such unit found
	free (value->unit);
	value->unit = NULL;
      }
    }
  }
}

/* The function goes through the property list of the given definition
   and checks whether there is the given property stored.  It returns
   NULL if not. */
static struct pair_t *
spice_find_property (struct definition_t * def, char * prop) {
  struct pair_t * pair;
  for (pair = def->pairs; pair != NULL; pair = pair->next) {
    if (!strcmp (pair->key, prop))
      return pair;
  }
  return NULL;
}

/* This function append all the required Qucs properties not given in
   the Spice netlist. */
static void spice_adjust_default_properties (struct definition_t * def) {
  struct define_t * entry = spice_get_qucs_definition (def);
  if (entry == NULL) return;

  // handle required properties
  for (int i = 0; PROP_IS_PROP (entry->required[i]); i++) {
    struct property_t * prop = &entry->required[i];
    if (spice_find_property (def, prop->key) == NULL) {
      struct pair_t * pair = create_pair ();
      pair->key = strdup (prop->key);
      pair->value = create_value ();
      if (PROP_IS_VAL (*prop)) {
	pair->value->value = prop->defaultval.d;
      }
      else {
	pair->value->ident = strdup (prop->defaultval.s);
      }
      def->pairs = netlist_append_pairs (def->pairs, pair);
    }
  }
}

// Helper structure for property translations and aliases in devives.
struct property_translation_t {
  char * key;
  char * trans;
}
property_translation[] = {
  { "CCS", "Cjs" },
  { "VA",  "Vaf" },
  { NULL, NULL }
};

/* The function translates property and aliases of devices in the list
   of key/value pairs of the given definition. */
void spice_adjust_alias_properties (struct pair_t * pair) {
  struct property_translation_t * prop = property_translation;
  for (; prop->key != NULL; prop++) {
    if (!strcasecmp (prop->key, pair->key)) {
      free (pair->key);
      pair->key = strdup (prop->trans);
    }
  }
}

/* The function goes through the pair (property) list of the given
   definition and adjusts the spelling of the property keys according
   to the Qucs definitions. */
void spice_adjust_properties (struct definition_t * def) {
  struct define_t * entry = spice_get_qucs_definition (def);
  if (entry) {
    struct pair_t * pair;
    for (pair = def->pairs; pair != NULL; pair = pair->next) {
      // handle required properties
      for (int i = 0; PROP_IS_PROP (entry->required[i]); i++) {
	if (!strcasecmp (entry->required[i].key, pair->key)) {
	  free (pair->key);
	  pair->key = strdup (entry->required[i].key);
	  break;
	}
      }
      // handle optional properties
      for (int i = 0; PROP_IS_PROP (entry->optional[i]); i++) {
	if (!strcasecmp (entry->optional[i].key, pair->key)) {
	  free (pair->key);
	  pair->key = strdup (entry->optional[i].key);
	  break;
	}
      }
      // some other direct translations
      spice_adjust_alias_properties (pair);
    }
  }
}

/* This is the overall Spice netlist translator.  It adjusts the list
   of definitions into usable structures. */
int spice_translator (void) {
  int error = 0;
  struct definition_t * def;
  for (def = definition_root; def != NULL; def = def->next) {
    if ((def->define = spice_find_definition (def->type)) != NULL) {
      strcpy (def->type, def->define->type);
      def->nodes = spice_get_nodes (def);
      def->pairs = spice_get_pairs (def);
      if (!strcasecmp (def->type, "Q")) {
	spice_translate_device (def);
      }
      spice_adjust_properties (def);
      spice_translate_nodes (def);
      spice_translate_units (def);
      spice_adjust_default_properties (def);
    }
    else {
      // skipped
    }
  }
  return error;
}

// My debugger...
void spice_lister (void) {
  struct definition_t * def;
  struct value_t * val;
  for (def = definition_root; def != NULL; def = def->next) {
    fprintf (stderr, "%s:%s", def->type, def->instance);
    for (val = def->values; val != NULL; val = val->next) {
      fprintf (stderr, " %s[%d]", val->ident, val->hint);
    }
    fprintf (stderr, "\n");
  }  
}

// Adjusts the hint value of the last entry in the value list.
void spice_set_last_hint (struct value_t * val, int hint) {
  if (val == NULL) return;
  for (; val->next != NULL; val = val->next);
  val->hint |= hint;
}

/* This function is the overall spice checker and translator. */
int spice_checker (void) {
  spice_lister ();
  return spice_translator ();
}
