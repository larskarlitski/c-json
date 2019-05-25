
#include <assert.h>
#include <ctype.h>
#include <c-stdaux.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include "c-json.h"

/*
 * TODO
 *
 *   - can read non-strings as object keys (treat as programmer error?)
 *   - should end_read() fail when there's still levels?
 *
 */

static int c_json_push_level(CJson *json, char state) {
        CJsonLevel *level;

        level = calloc(1, sizeof(CJsonLevel));
        if (!level)
                return -ENOTRECOVERABLE;

        level->parent = json->level;
        level->state = state;

        json->level = level;

        return 0;
}

static void c_json_pop_level(CJson *json) {
        CJsonLevel *parent;

        assert(json->level);

        parent = json->level->parent;
        free(json->level);
        json->level = parent;
}

static const char * skip_space(const char *p) {
        while (isspace(*p))
                p += 1;

        return p;
}

/*
 * Advances json->p to the start of the next value.
 */
static int c_json_advance(CJson *json) {
        if (_c_unlikely_(json->poison))
                return json->poison;

        json->p = skip_space(json->p);

        if (json->level) {
                switch (json->level->state) {
                        case '[':
                                if (*json->p == ',')
                                        json->p = skip_space(json->p + 1);
                                else if (*json->p != ']')
                                        return (json->poison = C_JSON_E_INVALID_JSON);
                                break;

                        case '{':
                                if (*json->p == ':') {
                                        json->level->state = ':';
                                        json->p = skip_space(json->p + 1);
                                }
                                else
                                        return (json->poison = C_JSON_E_INVALID_JSON);
                                break;

                        case ':':
                                if (*json->p == ',') {
                                        json->level->state = '{';
                                        json->p = skip_space(json->p + 1);
                                } else if (*json->p != '}')
                                        return (json->poison = C_JSON_E_INVALID_JSON);
                                break;
                }
        }

        return 0;
}

static int c_json_read_unicode_char(const char *p, FILE *stream) {
        uint8_t digits[4];
        uint16_t cp;

        for (size_t i = 0; i < 4; i += 1) {
                switch (p[i]) {
                        case '0' ... '9':
                                digits[i] = p[i] - '0';
                                break;

                        case 'a' ... 'f':
                                digits[i] = p[i] - 'a' + 0x0a;
                                break;

                        case 'A' ... 'F':
                                digits[i] = p[i] - 'A' + 0x0a;
                                break;

                        default:
                                return C_JSON_E_INVALID_JSON;
                }
        }

        cp = digits[0] << 12 | digits[1] << 8 | digits[2] << 4 | digits[3];

        if (cp <= 0x007f) {
                fputc((char)cp, stream);
        } else if (cp <= 0x07ff) {
                fputc((char)(0xc0 | (cp >> 6)), stream);
                fputc((char)(0x80 | (cp & 0x3f)), stream);
        } else {
                fputc((char)(0xe0 | (cp >> 12)), stream);
                fputc((char)(0x80 | ((cp >> 6) & 0x3f)), stream);
                fputc((char)(0x80 | (cp & 0x3f)), stream);
        }

        return 0;
}

_c_public_ void c_json_init(CJson *json) {
        *json = (CJson)C_JSON_INIT;
}

_c_public_ void c_json_deinit(CJson *json) {
        while (json->level)
                c_json_pop_level(json);

        *json = (CJson)C_JSON_INIT;
}

_c_public_ int c_json_peek(CJson *json) {
        switch (*json->p) {
                case '[':
                case ']':
                        return C_JSON_TYPE_ARRAY;

                case '{':
                case '}':
                        return C_JSON_TYPE_OBJECT;

                case '"':
                        return C_JSON_TYPE_STRING;

                case '0' ... '9':
                case '-':
                        return C_JSON_TYPE_NUMBER;

                case 't':
                case 'f':
                        return C_JSON_TYPE_BOOLEAN;

                case 'n':
                        return C_JSON_TYPE_NULL;

                default:
                        return -1;
        }
}

_c_public_ void c_json_begin_read(CJson *json, const char *string) {
        c_json_deinit(json);

        json->input = string;
        json->p = skip_space(json->input);
}

_c_public_ int c_json_end_read(CJson *json) {
        int r = json->poison;

        if (json->level)
                r = C_JSON_E_INVALID_TYPE;

        c_json_deinit(json);

        return r;
}

_c_public_ int c_json_read_string(CJson *json, char **stringp) {
        _c_cleanup_(c_fclosep) FILE *stream = NULL;
        _c_cleanup_(c_freep) char *string = NULL;
        size_t size;
        int r;

        if (_c_unlikely_(json->poison))
                return json->poison;

        if (*json->p != '"')
                return (json->poison = C_JSON_E_INVALID_TYPE);

        stream = open_memstream(&string, &size);
        if (!stream)
                return (json->poison = -ENOTRECOVERABLE);

        json->p += 1;
        while (*json->p != '"') {
                if (*json->p == '\0')
                        return (json->poison = C_JSON_E_INVALID_JSON);

                if (*json->p == '\\') {
                        json->p += 1;
                        switch (*json->p) {
                                case '"':
                                        if (fputc('"', stream) < 0)
                                                return (json->poison = -ENOTRECOVERABLE);
                                        break;

                                case '\\':
                                        if (fputc('\\', stream) < 0)
                                                return (json->poison = -ENOTRECOVERABLE);
                                        break;

                                case '/':
                                        if (fputc('/', stream) < 0)
                                                return (json->poison = -ENOTRECOVERABLE);
                                        break;

                                case 'b':
                                        if (fputc('\b', stream) < 0)
                                                return (json->poison = -ENOTRECOVERABLE);
                                        break;

                                case 'f':
                                        if (fputc('\f', stream) < 0)
                                                return (json->poison = -ENOTRECOVERABLE);
                                        break;

                                case 'n':
                                        if (fputc('\n', stream) < 0)
                                                return (json->poison = -ENOTRECOVERABLE);
                                        break;

                                case 'r':
                                        if (fputc('\r', stream) < 0)
                                                return (json->poison = -ENOTRECOVERABLE);
                                        break;

                                case 't':
                                        if (fputc('\t', stream) < 0)
                                                return (json->poison = -ENOTRECOVERABLE);
                                        break;

                                case 'u': {
                                        int r;

                                        r = c_json_read_unicode_char(json->p + 1, stream);
                                        if (r)
                                                return (json->poison = r);
                                        json->p += 4;
                                        break;
                                }

                                default:
                                        return (json->poison = C_JSON_E_INVALID_JSON);
                        }

                } else if (fputc(*json->p, stream) < 0)
                        return (json->poison = -ENOTRECOVERABLE);

                json->p += 1;
        }

        json->p += 1; /* '"' */

        stream = c_fclose(stream);

        r = c_json_advance(json);
        if (r)
                return r;

        if (stringp) {
                *stringp = string;
                string = NULL;
        }

        return 0;
}

_c_public_ int c_json_read_u64(CJson *json, uint64_t *numberp) {
        char *end;
        uint64_t number;
        int r;

        if (_c_unlikely_(json->poison))
                return json->poison;

        /* strtoul() silently flips sign if first char is a minus */
        if (*json->p == '-')
                return (json->poison = C_JSON_E_INVALID_TYPE);

        number = strtoul(json->p, &end, 10);

        if (end == json->p || *end == '.' || *end == 'e' || *end == 'E')
                return (json->poison = C_JSON_E_INVALID_TYPE);

        json->p = end;

        r = c_json_advance(json);
        if (r)
                return r;

        if (numberp)
                *numberp = number;

        return 0;
}

_c_public_ int c_json_read_f64(CJson *json, double *numberp) {
        char *end;
        double number;
        locale_t loc;
        int r;

        if (_c_unlikely_(json->poison))
                return json->poison;

        loc = newlocale(LC_NUMERIC_MASK, "C", (locale_t) 0);
        uselocale(loc);
        number = strtod(json->p, &end);
        freelocale(loc);

        json->p = end;

        r = c_json_advance(json);
        if (r)
                return r;

        if (numberp)
                *numberp = number;

        return 0;
}

_c_public_ int c_json_read_bool(CJson *json, bool *boolp) {
        bool b;
        int r;

        if (_c_unlikely_(json->poison))
                return json->poison;

        if (!strncmp(json->p, "true", strlen("true"))) {
                b = true;
                json->p += strlen("true");
        } else if (!strncmp(json->p, "false", strlen("false"))) {
                b = false;
                json->p += strlen("false");
        } else
                return (json->poison = C_JSON_E_INVALID_TYPE);

        r = c_json_advance(json);
        if (r)
                return r;

        if (boolp)
                *boolp = b;

        return 0;
}

_c_public_ bool c_json_more(CJson *json) {
        if (!*json->p)
                return false;

        if (json->level) {
                if (json->level->state == '[')
                        return *json->p != ']';

                if (json->level->state == '{')
                        return *json->p != '}';
        }

        return true;
}

_c_public_ int c_json_open_array(CJson *json) {
        if (_c_unlikely_(json->poison))
                return json->poison;

        if (*json->p != '[')
                return (json->poison = C_JSON_E_INVALID_TYPE);

        json->p = skip_space(json->p + 1);
        c_json_push_level(json, '[');

        return 0;
}

_c_public_ int c_json_close_array(CJson *json) {
        if (_c_unlikely_(json->poison))
                return json->poison;

        if (*json->p != ']')
                return (json->poison = C_JSON_E_INVALID_TYPE);

        json->p += 1;
        c_json_pop_level(json);

        return c_json_advance(json);
}

_c_public_ int c_json_open_object(CJson *json) {
        if (_c_unlikely_(json->poison))
                return json->poison;

        if (*json->p != '{')
                return (json->poison = C_JSON_E_INVALID_TYPE);

        json->p = skip_space(json->p + 1);
        c_json_push_level(json, '{');

        return 0;
}

_c_public_ int c_json_close_object(CJson *json) {
        if (_c_unlikely_(json->poison))
                return json->poison;

        if (*json->p != '}')
                return (json->poison = C_JSON_E_INVALID_TYPE);

        json->p += 1;
        c_json_pop_level(json);

        return c_json_advance(json);
}