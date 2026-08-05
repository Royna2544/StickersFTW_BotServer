# Stickers FTW (For Telegram WhatsApp) Telegram Bot server

## A simple bot server for downloading stickers from Telegram using its bot API.

### API (v1)
1. GET /set/:sticker_set_name:/ - Get a sticker set, and information about it by its name. (e.g. stickers count, title, etc.).
2. GET /set/:sticker_set_name:/:id:/ - Download a sticker from a sticker set by set's name and id.
3. GET /set/:sticker_set_name:/:id:/thumbnail - Get the thumbnail of a sticker by sticker's name and id.

## Response codes
- 200 - OK
- 400 - Bad Request (e.g. invalid sticker set name, invalid sticker id, etc.)
- 403 - Forbidden (e.g. Telegram API token is invalid, etc.)
- 404 - Not Found (e.g. sticker set not found, sticker not found, etc.)
- 429 - Too Many Requests (e.g. Telegram API rate limit exceeded, etc.)
- 500 - Internal Server Error (e.g. Telegram API error, download error, etc.)

## Process arguments
- `--port` - The port to run the server on. Default is 8080.
- `--host` - The host to run the server on. Default is localhost.
- `--token` - The Telegram bot token to use for the API. Required.
- `--log-level` - The log level to use for the server. Default is info. Options are: debug, info, warning, error, critical.
- `--server` - The server to use for the API. Default is https://api.telegram.org.

## Response format
1. For GET /set/:sticker_set_name:/ - Returns a JSON object with the following fields:
   `name` - The name of the sticker set.
   `title` - The title of the sticker set.
   `stickers` - An array of sticker objects, each with the following fields:
	  `id` - The id of the sticker.
	  `width` - The width of the sticker.
	  `height` - The height of the sticker.
	  `size` - The size of the sticker in bytes. (if available)
	  `thumb` - The thumbnail of the sticker (if available).
	  `emoji` - Emoji associated with the sticker (if available).

2/3. For GET /set/:sticker_set_name:/:id:/ and GET /set/:sticker_set_name:/:id:/thumbnail - Returns the sticker file or thumbnail file as a binary stream.