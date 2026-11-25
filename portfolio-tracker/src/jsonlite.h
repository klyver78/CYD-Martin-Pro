// Minimal read-only JSON scanner.
//
// The tracker only ever reads a handful of known fields out of responses it did not design, so a
// full DOM parser would cost more RAM (and a library dependency) than it is worth. These helpers
// walk the raw text instead: no allocation, no copying, and they compile unchanged on the PC, so
// the simulator can run them against real recorded API responses.
//
// Everything is tolerant by design: a missing or malformed field just reports failure.
#pragma once
#include <stdlib.h>
#include <string.h>

// Skip whitespace.
inline const char* jlWs(const char* p) {
  while (p && *p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
  return p;
}

// End of the string/array/object/value starting at p (p points at the first character of the
// value). Returns a pointer just past it, or nullptr if the text is truncated.
inline const char* jlSkipValue(const char* p) {
  p = jlWs(p);
  if (!p || !*p) return nullptr;
  if (*p == '"') {
    for (++p; *p; ++p) {
      if (*p == '\\' && p[1]) { ++p; continue; }
      if (*p == '"') return p + 1;
    }
    return nullptr;
  }
  if (*p == '{' || *p == '[') {
    int depth = 0;
    for (; *p; ++p) {
      if (*p == '"') {
        const char* q = jlSkipValue(p);
        if (!q) return nullptr;
        p = q - 1;
        continue;
      }
      if (*p == '{' || *p == '[') ++depth;
      else if (*p == '}' || *p == ']') { if (--depth == 0) return p + 1; }
    }
    return nullptr;
  }
  while (*p && *p != ',' && *p != '}' && *p != ']') ++p;   // number / true / false / null
  return p;
}

// First occurrence of "key": inside [p, end) at any depth; returns a pointer to the value.
// `end` may be nullptr for "to the end of the string".
inline const char* jlFind(const char* p, const char* end, const char* key) {
  if (!p) return nullptr;
  size_t kl = strlen(key);
  for (; *p && (!end || p < end); ++p) {
    if (*p != '"' || strncmp(p + 1, key, kl) != 0 || p[1 + kl] != '"') continue;
    const char* q = jlWs(p + 2 + kl);
    if (*q != ':') continue;
    q = jlWs(q + 1);
    return (end && q >= end) ? nullptr : q;
  }
  return nullptr;
}

// The object/array that "key" maps to, as a [begin, end) range.
inline bool jlFindRange(const char* p, const char* end, const char* key,
                        const char*& outBegin, const char*& outEnd) {
  const char* v = jlFind(p, end, key);
  if (!v) return false;
  const char* e = jlSkipValue(v);
  if (!e) return false;
  outBegin = v;
  outEnd = e;
  return true;
}

// Copy a JSON string value (handles \" \\ \/ \n \t and \uXXXX -> '?' for non-ASCII).
inline bool jlStr(const char* p, char* out, size_t cap) {
  if (!p || *p != '"' || cap == 0) return false;
  size_t i = 0;
  for (++p; *p && *p != '"'; ++p) {
    char c = *p;
    if (c == '\\') {
      ++p;
      switch (*p) {
        case 'n': c = '\n'; break;
        case 't': c = ' ';  break;
        case 'r': continue;
        case 'u': {
          unsigned v = 0;
          for (int k = 1; k <= 4 && p[k]; ++k) {
            char h = p[k];
            v = v * 16 + (unsigned)(h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
          }
          p += 4;
          c = (v >= 32 && v < 127) ? (char)v : '?';
          break;
        }
        case '\0': out[i] = 0; return false;
        default: c = *p;
      }
    }
    if (i + 1 < cap) out[i++] = c;
  }
  out[i] = 0;
  return *p == '"';
}

// Read a number; `null` yields `missing` (useful for Yahoo's holiday gaps).
inline bool jlNum(const char* p, float& out, float missing = 0.0f) {
  if (!p) return false;
  if (!strncmp(p, "null", 4)) { out = missing; return true; }
  char* e = nullptr;
  float v = strtof(p, &e);
  if (e == p) return false;
  out = v;
  return true;
}

// Read an array of numbers into out[] (at most cap). `null` entries become `missing`.
// Returns the number of entries read, or -1 if p is not an array.
inline int jlNumArray(const char* p, float* out, int cap, float missing = 0.0f) {
  if (!p || *p != '[') return -1;
  int n = 0;
  p = jlWs(p + 1);
  while (*p && *p != ']') {
    float v;
    if (!jlNum(p, v, missing)) return n;
    if (n < cap) out[n] = v;
    ++n;
    p = jlSkipValue(p);
    if (!p) return n;
    p = jlWs(p);
    if (*p == ',') p = jlWs(p + 1);
  }
  return n;
}

// Same for an array of 64-bit-ish integers (Unix timestamps fit in a long).
inline int jlLongArray(const char* p, long* out, int cap) {
  if (!p || *p != '[') return -1;
  int n = 0;
  p = jlWs(p + 1);
  while (*p && *p != ']') {
    char* e = nullptr;
    long v = strtol(p, &e, 10);
    if (e == p) v = 0;                      // null
    if (n < cap) out[n] = v;
    ++n;
    p = jlSkipValue(p);
    if (!p) return n;
    p = jlWs(p);
    if (*p == ',') p = jlWs(p + 1);
  }
  return n;
}

// Iterate the elements of an array: pass nullptr to start, then the previous element.
inline const char* jlArrayNext(const char* arrBegin, const char* prev) {
  if (!arrBegin || *arrBegin != '[') return nullptr;
  const char* p;
  if (!prev) {
    p = jlWs(arrBegin + 1);
  } else {
    p = jlSkipValue(prev);
    if (!p) return nullptr;
    p = jlWs(p);
    if (*p != ',') return nullptr;
    p = jlWs(p + 1);
  }
  return (*p && *p != ']') ? p : nullptr;
}

// The value of a top-level member of an object, matched exactly (so "AAPL": is found but
// "symbol":"AAPL" is not). Used to pull one symbol out of the spark response.
inline bool jlMember(const char* objBegin, const char* objEnd, const char* name,
                     const char*& outBegin, const char*& outEnd) {
  if (!objBegin || *objBegin != '{') return false;
  size_t nl = strlen(name);
  const char* p = jlWs(objBegin + 1);
  while (p && p < objEnd && *p == '"') {
    const char* keyEnd = jlSkipValue(p);
    if (!keyEnd) return false;
    bool hit = (size_t)(keyEnd - p) == nl + 2 && !strncmp(p + 1, name, nl);
    const char* v = jlWs(keyEnd);
    if (*v != ':') return false;
    v = jlWs(v + 1);
    const char* e = jlSkipValue(v);
    if (!e) return false;
    if (hit) { outBegin = v; outEnd = e; return true; }
    p = jlWs(e);
    if (*p == ',') p = jlWs(p + 1);
  }
  return false;
}
