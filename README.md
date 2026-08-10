# Stickers FTW (For Telegram WhatsApp) Telegram Bot server

## A simple bot server for downloading stickers from Telegram using its bot API, and for pushing new sticker packs to Telegram.

### API (v1)
All routes are under `/v1`. A trailing slash is optional on every route (both `/v1/set/name` and `/v1/set/name/` work).

1. `GET /v1/set/:sticker_set_name` — Get a sticker set, and information about it, by its name (e.g. stickers count, title, etc.).
2. `GET /v1/set/:sticker_set_name/:id` — Download a sticker from a sticker set by set's name and id.
3. `GET /v1/set/:sticker_set_name/:id/thumbnail` — Get the thumbnail of a sticker by sticker's name and id.
4. `GET /v1/bot` — Get information about the bot itself (currently just its username). Purely informational, useful for client UIs that need to show the user which bot to message.
5. `POST /v1/set/:sticker_set_name` — Push a sticker to Telegram: appends it to an existing set the caller owns, or creates a brand-new set if `title` is supplied and the set doesn't exist yet. See "Push request format" below.

## Response codes
- 200 - OK
- 201 - Created (push endpoint only: a brand-new sticker set was created)
- 400 - Bad Request (e.g. invalid sticker set name, invalid sticker id, missing/invalid push fields, etc.)
- 404 - Not Found (e.g. sticker set not found, sticker not found, etc.)
- 429 - Too Many Requests (e.g. Telegram API rate limit exceeded, etc.)
- 500 - Internal Server Error (e.g. Telegram API error, download error, etc.)

Note: 403 is reserved but never currently emitted by this server.

## Process arguments
- `--port` - The port to run the server on. Default is 8080.
- `--host` - The host to run the server on. Default is localhost.
- `--token` - The Telegram bot token to use for the API. Required, unless `STICKERSFTW_TOKEN` is set in the environment. Prefer the environment variable when running as a service: arguments are visible to any local user via `ps`.
- `--log-level` - The log level to use for the server. Default is info. Options are: debug, info, warning, error, critical.
- `--server` - The server to use for the API. Default is https://api.telegram.org.

## Response format

### `GET /v1/set/:sticker_set_name`
Returns a JSON object with the following fields:
- `name` - The name of the sticker set.
- `title` - The title of the sticker set.
- `stickers` - An array of sticker objects, each with the following fields:
  - `id` - The id of the sticker (Telegram's `file_unique_id`; opaque, stable, used in the other routes).
  - `width` - The width of the sticker.
  - `height` - The height of the sticker.
  - `size` - The size of the sticker in bytes. Always present; `0` if Telegram didn't report a size.
  - `thumb` - The sticker's thumbnail identifier (if available). This is an opaque id, **not** a URL — it does not correspond to any fetchable route on its own.
  - `emoji` - Emoji associated with the sticker (if available).

There is no field describing the sticker's format (static/animated/video) — the only way to determine that is the `Content-Type` of the binary download routes below, which is sniffed from the file's actual magic bytes:
- `image/webp` — static sticker
- `video/webm` — video sticker
- `application/x-tgsticker` — animated sticker (gzip-compressed Lottie JSON, a `.tgs` file)
- `image/jpeg` — occasionally used for thumbnails
- `application/octet-stream` — unrecognized format

### `GET /v1/set/:sticker_set_name/:id` and `GET /v1/set/:sticker_set_name/:id/thumbnail`
Return the sticker file or thumbnail file as a binary stream, with `Content-Type` set per the sniffing rules above.

### `GET /v1/bot`
Returns a JSON object: `{"username": "<bot_username>"}`.

### `POST /v1/set/:sticker_set_name`
Pushes one sticker to Telegram. Request body is `multipart/form-data` with the following parts:
- `user_id` (required) - The numeric Telegram user id that will own the sticker set. This user **must have already started a conversation with the bot** (sent it `/start` at least once) — Telegram rejects sticker-set operations for a `user_id` it hasn't seen before, and this server has no way to work around that.
- `format` (required) - Either `static` or `video`. There is no support for pushing true Lottie/`.tgs` animated stickers — that format has to be authored, not derived from an arbitrary image or video clip.
- `emojis` (required) - A comma-separated list of 1 to 3 emoji to associate with the sticker.
- `sticker` (required) - The sticker file itself, already sized/encoded to Telegram's requirements for the chosen format.
- `title` (required only when creating a brand-new set) - The human-readable title for the sticker set. If the named set doesn't exist yet and no `title` is supplied, the request fails with 400.

`:sticker_set_name` here is the *short* name only (letters, digits, underscores, must start with a letter) — the server appends `_by_<bot_username>` itself to build Telegram's real, globally-unique set name, and returns that full name in the response.

On success, responds with the same JSON shape as `GET /v1/set/:sticker_set_name` (the freshly re-fetched, updated set) — `201` if a new set was created, `200` if the sticker was appended to an existing one.

## Error response format
Every non-2xx response (all routes above) now returns a JSON body:
```json
{"error_code": 400, "description": "..."}
```
`description` is Telegram's own error text where available (e.g. `STICKERSET_INVALID`, `PEER_ID_INVALID` when the owning user hasn't started the bot), which callers can use to show a meaningful message instead of just a status code.
