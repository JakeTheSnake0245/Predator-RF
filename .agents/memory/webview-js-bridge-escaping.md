---
name: WebView JS bridge escaping
description: How to safely pass JSON into evaluateJavascript on Android without injection/syntax breaks.
---

# WebView JS bridge escaping

Never embed JSON into `evaluateJavascript("fn('$json')")` with manual
single-quote `replace()` — it misses backslashes and peer/track-controlled
names can break the literal or inject JS.

**Why:** architect review caught a bridge where `replace("'", "\'")` was a
no-op; peer names come from remote units, so this was an injection surface.

**How to apply:** build the literal with `JSONObject.quote(jsonString)`
(fully escaped, double-quoted) and interpolate it unquoted:
`"fn($jsLiteral);"`. The JS side already accepts string-or-object via
`JSON.parse` guards.
