/* Just enough JSON to read flat objects.
 *
 * cJSON is not in the IDF v6.1 core any more -- it moved to the component
 * manager, which needs a network fetch this bench cannot reach directly. The
 * messages here are handfuls of scalars in a flat object, so a ~100 line
 * reader beats a dependency: it cannot fail to resolve, and it cannot pull in
 * a component whose version drifts under us.
 *
 * Scope, stated so nobody is surprised: top-level keys, string / number /
 * bool / object values, no arrays, no escapes other than \" and \\ .
 * Anything else returns false and the caller keeps its previous value. */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Finds "key" at the top level of JSON (the object itself, or any object text
 * passed in) and copies its string value into OUT. Returns false when the key
 * is absent or not a string. */
bool jh_string(const char *json, const char *key, char *out, size_t outsz);

/* Same, for a number. */
bool jh_number(const char *json, const char *key, double *out);

/* Same, for true/false. A JSON null reads as false with found=true, which is
 * what an absent command looks like. */
bool jh_bool(const char *json, const char *key, bool *out);

/* Pointer to the inner text of a nested object value, or NULL. The pointer is
 * into JSON and stays valid as long as it does. */
const char *jh_object(const char *json, const char *key);

/* True when the key exists at top level with any value (including null). */
bool jh_has(const char *json, const char *key);
